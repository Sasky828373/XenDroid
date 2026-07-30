package xendroid.compose.data

import android.content.Context
import android.util.Log
import xendroid.compose.core.ContentPaths
import xendroid.compose.core.GameMetadataSource
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.firstOrNull
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.File

/**
 * Discovers games by walking ONE level of the persisted real-path (All Files Access)
 * games dir (no recursion). GOD metadata + icons come from native; ISO/ZAR/XEX get
 * filename-derived names and the app_icon fallback.
 */
class GameLibraryRepository(
    private val appContext: Context,
    private val prefs: PreferencesStore,
    private val metadata: GameMetadataSource,
    private val iconCache: IconCache,
    private val metadataCache: GameMetadataCache,
) {
    private val tag = "GameLibraryRepo"

    /** Serializes scans so two overlapping refresh()es don't both cold-extract (and race on
     *  the cache file): the second waits, then runs cheap against the now-warm cache. */
    private val scanMutex = Mutex()

    /** Result of a scan: distinguishes "no folder" / "grant lost" from a real list. */
    sealed interface ScanResult {
        data object NoFolder : ScanResult
        data object PermissionLost : ScanResult
        data class Games(val games: List<Game>) : ScanResult
    }

    /** Persist a real-path (All Files Access) games dir. Validates the path is a readable
     *  directory FIRST so a later scan can't fail PermissionLost on an unreadable dir.
     *  Returns whether it was saved. */
    suspend fun saveGameDirPath(path: String): Boolean = withContext(Dispatchers.IO) {
        val dir = File(path)
        if (!dir.isDirectory || dir.listFiles() == null) {
            Log.w(tag, "saveGameDirPath rejected (not a readable dir): $path")
            return@withContext false
        }
        prefs.setGameDirPath(path)
        true
    }

    /** The persisted real-path games dir (absolute host path), or null. */
    suspend fun currentGameDirPath(): String? = withContext(Dispatchers.IO) {
        prefs.gameDirPath.firstOrNull()
    }

    suspend fun scan(): ScanResult = withContext(Dispatchers.IO) {
        // Serialize scans so two overlapping refresh()es can't both cold-extract / race on
        // the cache file; the second waits then runs cheap against the now-warm cache.
        scanMutex.withLock { scanLocked() }
    }

    private suspend fun scanLocked(): ScanResult {
        val dir = prefs.gameDirPath.firstOrNull() ?: return ScanResult.NoFolder
        return scanRealPathLocked(dir)
    }

    /** Resolve a game's 8-char uppercase-hex title id for the per-game config path. GOD reads the
     *  container header; ISO/ZAR mount the disc + read default.xex's XEX header; XEX_FOLDER reads
     *  default.xex directly (all boot-free via title_id_from_path / ExtractZarMetadata).
     *  MUST run off the main thread (these mmap the file). [Game.launchUri] is an absolute path. */
    suspend fun readTitleId(ctx: Context, game: Game): String? = withContext(Dispatchers.IO) {
        game.titleId?.takeIf { it.isNotBlank() && it != "00000000" }?.let { return@withContext it }
        when (game.format) {
            GameFormat.GOD -> metadata.readGodPath(game.launchUri)?.titleId
            GameFormat.STFS -> metadata.readContentHeader(game.launchUri)?.titleId
            GameFormat.ISO, GameFormat.XEX_FOLDER, GameFormat.ZAR ->
                metadata.readTitleIdPath(game.launchUri, game.format)
        }
    }

    /** Real-path file change signature (size + mtime). A real File gives reliable length()
     *  + lastModified(), so these are normally cacheable. */
    private fun signatureOf(file: File): GameMetadataCache.Signature =
        GameMetadataCache.Signature(sizeBytes = file.length(), lastModified = file.lastModified())

    /** Cache lookup for the extracting branches: a [GameMetadataCache.Decision.Hit]'s
     *  values when the [launchUri] entry is fresh (signature matches + icon File survives),
     *  else null (caller extracts). The icon-existence check uses the real on-disk file. */
    private fun metadataCacheHit(
        launchUri: String,
        signature: GameMetadataCache.Signature,
    ): GameMetadataCache.Decision.Hit? {
        val decision = GameMetadataCache.decide(
            cached = metadataCache.get(launchUri),
            signature = signature,
            iconFileExists = { iconCache.fileFor(it).exists() },
        )
        return decision as? GameMetadataCache.Decision.Hit
    }

    private data class Extracted(
        val name: String,
        val iconCacheName: String?,
        val titleId: String?,
        val mediaId: String?,
        val discNumber: Int = 0,
        val discCount: Int = 0,
    )

    /** Shared cache wrapper for the always-producing extracting branches (ISO, XEX_FOLDER,
     *  ZAR): reuse the cached values on a fresh HIT; otherwise run [extract], cache its result,
     *  and build the Game. */
    private inline fun cachedOrExtract(
        launchUri: String,
        signature: GameMetadataCache.Signature,
        format: GameFormat,
        extract: () -> Extracted,
    ): Game {
        metadataCacheHit(launchUri, signature)?.let {
            return Game(launchUri, it.name, format, it.iconCacheName, it.titleId, it.mediaId,
                        it.discNumber, it.discCount)
        }
        val e = extract()
        metadataCache.put(launchUri, e.name, e.iconCacheName, signature, e.titleId, e.mediaId,
                          format, e.discNumber, e.discCount)
        return Game(launchUri, e.name, format, e.iconCacheName, e.titleId, e.mediaId,
                    e.discNumber, e.discCount)
    }

    // ---- Real-path (All Files Access) scan: a java.io.File walk using real paths + the
    // path-based native metadata (the core's real-path devices mount directly from a path).

    /** Walk ONE level of the real-path games dir, classify each child, sort. Null children
     *  (dir became unreadable -- grant revoked) -> [ScanResult.PermissionLost]. Reuses the
     *  shared metadata + icon caches. */
    private fun scanRealPathLocked(dirPath: String): ScanResult {
        val children = File(dirPath).listFiles() ?: return ScanResult.PermissionLost

        // Load the per-game extraction cache once; mutate during classify; persist once after.
        metadataCache.load()
        val games = buildList {
            for (child in children) {                      // ONE level only
                classifyPath(child)?.let { add(it) }
            }
        }.sortedBy { it.name.lowercase() }
        metadataCache.retainOnly(games.mapTo(HashSet()) { it.launchUri })
        metadataCache.save()
        return ScanResult.Games(games)
    }

    /** One real-path child -> a Game, or null if ignored/unparseable. launchUri = the
     *  absolute path; metadata comes from the path-based native probes. */
    private fun classifyPath(child: File): Game? {
        val name = child.name
        if (child.isDirectory) {
            // XEX folder: launch the default.xex CHILD (case-insensitive). One native
            // decompress yields the real XDBF title + icon; fall back to the FOLDER name.
            val xex = child.listFiles()?.firstOrNull {
                it.isFile && it.name.equals("default.xex", ignoreCase = true)
            } ?: return null
            val xexPath = xex.absolutePath
            // Signature off the default.xex CHILD (its bytes are what extraction reads).
            return cachedOrExtract(xexPath, signatureOf(xex), GameFormat.XEX_FOLDER) {
                val meta = metadata.readXexMetaPath(xexPath, GameFormat.XEX_FOLDER)
                val displayName = meta?.name?.takeIf { it.isNotEmpty() } ?: name
                val iconName = meta?.iconPng?.let { iconCache.write(xexPath, it) }
                Extracted(displayName, iconName, meta?.titleId, meta?.mediaId,
                          meta?.discNumber ?: 0, meta?.discCount ?: 0)
            }
        }
        return when (GameFormat.fromFileName(name)) {
            GameFormat.ISO, GameFormat.ZAR -> {
                val fmt = GameFormat.fromFileName(name)!!
                val path = child.absolutePath
                cachedOrExtract(path, signatureOf(child), fmt) {
                    val meta = metadata.readXexMetaPath(path, fmt)
                    val displayName = meta?.name?.takeIf { it.isNotEmpty() }
                        ?: fmt.displayNameFor(name)
                    val iconName = meta?.iconPng?.let { iconCache.write(path, it) }
                    Extracted(displayName, iconName, meta?.titleId, meta?.mediaId,
                          meta?.discNumber ?: 0, meta?.discCount ?: 0)
                }
            }
            GameFormat.GOD -> classifyExtensionless(child, name)
            GameFormat.STFS, GameFormat.XEX_FOLDER, null -> null
        }
    }

    /** An extensionless file: a GOD container, an STFS launchable-game container, or neither
     *  (dropped). Unlike cachedOrExtract this distinguishes "not a game" (return null) from a
     *  cache miss, and carries the format through the cache so a hit rebuilds the right Game.
     *  Probe order matches the scan: GOD (readGodPath) first, then an STFS content-header probe. */
    private fun classifyExtensionless(child: File, name: String): Game? {
        val path = child.absolutePath
        metadataCacheHit(path, signatureOf(child))?.let { hit ->
            val fmt = hit.format ?: GameFormat.GOD   // legacy entries predate STFS -> GOD
            return Game(path, hit.name, fmt, hit.iconCacheName, hit.titleId, hit.mediaId,
                        hit.discNumber, hit.discCount)
        }
        metadata.readGodPath(path)?.let { meta ->
            val displayName = meta.name.ifEmpty { name }
            val iconName = meta.iconPng?.let { iconCache.write(path, it) }
            metadataCache.put(path, displayName, iconName, signatureOf(child), meta.titleId, meta.mediaId,
                              GameFormat.GOD, meta.discNumber, meta.discCount)
            return Game(path, displayName, GameFormat.GOD, iconName, meta.titleId, meta.mediaId,
                        meta.discNumber, meta.discCount)
        }
        // GOD MUST be probed first (above): a GOD container also parses as a content
        // package, so reaching here only after readGodPath fails keeps GOD out of this
        // STFS branch. What's left is an STFS launchable-game container (XBLA/arcade, etc.).
        val header = metadata.readContentHeader(path) ?: return null
        if (!ContentPaths.isLaunchableGameType(header.contentType)) return null  // add-on content, not a game
        val displayName = header.displayName.ifBlank { name }
        val iconName = header.iconPng?.let { iconCache.write(path, it) }
        metadataCache.put(path, displayName, iconName, signatureOf(child), header.titleId, null, GameFormat.STFS)
        return Game(path, displayName, GameFormat.STFS, iconName, header.titleId, mediaId = null)
    }
}
