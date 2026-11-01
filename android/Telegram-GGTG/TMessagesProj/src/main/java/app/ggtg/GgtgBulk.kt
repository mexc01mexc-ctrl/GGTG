package app.ggtg

import kotlinx.coroutines.delay
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withTimeoutOrNull
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.FileLoader
import org.telegram.messenger.MessagesController
import org.telegram.messenger.MessagesStorage
import org.telegram.messenger.NotificationCenter
import org.telegram.messenger.UserConfig
import org.telegram.tgnet.TLRPC
import org.telegram.tgnet.tl.TL_account

// ng: Kotlin port of the account-side steps of settings/bulk_accounts_actions.cpp - runs one
// action over the chosen accounts strictly one at a time, with a random pause between them, all
// logged to a GgtgProcess. Photo upload is out of scope for now; the text/session/folder steps
// are here. Each step returns (ok, message).
object GgtgBulk {

    @Volatile var cancelled = false

    private const val PAUSE_MIN = 20_000L
    private const val PAUSE_MAX = 30_000L

    sealed class Action(val title: String) {
        class SetName(val first: String, val last: String) : Action("Имя и фамилия")
        class SetBio(val about: String) : Action("Описание")
        class SetUsernameAuto : Action("Юзернейм (авто)")
        object RemoveUsername : Action("Удалить юзернейм")
        object TerminateOtherSessions : Action("Деактивировать другие сессии")
        class MoveToFolder(val folder: String) : Action("Перенос в папку")
        /** Photos are handed out round-robin over the chosen accounts (3 photos, 5 accounts: 1-2-3-1-2). */
        class SetPhoto(val paths: List<String>, val replace: Boolean) : Action("Фото")
    }

    /** Runs [action] over [slots], one at a time; progress in "Процессы". */
    suspend fun run(slots: List<Int>, action: Action) {
        GgtgAccounts.wake(slots)
        val process = GgtgProcess.start("Массовое действие: ${action.title} (${slots.size})", GgtgProcess.Kind.Bulk)
        var done = 0
        cancelled = false
        for ((index, slot) in slots.withIndex()) {
            if (cancelled) { process.log("Остановлено пользователем"); break }
            val label = GgtgAccounts.label(slot)
            process.progress(index, slots.size)
            process.log("[$label] выполняется...")
            val (ok, message) = try {
                step(slot, action, index)
            } catch (t: Throwable) {
                false to (t.message ?: "ошибка")
            }
            process.log("[$label] " + if (ok) "успешно" + (if (message.isEmpty()) "" else " ($message)") else "ошибка: " + message.ifEmpty { "неизвестная" })
            if (ok) done++
            process.progress(index + 1, slots.size)
            if (index < slots.size - 1 && !cancelled) delay(PAUSE_MIN + (0 until (PAUSE_MAX - PAUSE_MIN)).random())
        }
        process.succeed("Выполнено: $done из ${slots.size}" + if (cancelled) " (остановлено)" else "")
    }

    private suspend fun step(slot: Int, action: Action, index: Int): Pair<Boolean, String> = when (action) {
        is Action.SetPhoto -> changePhoto(slot, action.paths[index % action.paths.size], action.replace)
        is Action.SetName -> updateProfile(slot, action.first, action.last, null)
        is Action.SetBio -> updateProfile(slot, null, null, action.about)
        is Action.SetUsernameAuto -> {
            val user = GgtgAccounts.user(slot)
            val first = firstAsciiLetter(user?.first_name, 'u')
            val last = firstAsciiLetter(user?.last_name, 'x')
            updateUsername(slot, "$first$last${UserConfig.getInstance(slot).clientUserId}")
        }
        Action.RemoveUsername -> updateUsername(slot, "")
        Action.TerminateOtherSessions -> resetAuthorizations(slot)
        is Action.MoveToFolder -> {
            val uc = UserConfig.getInstance(slot)
            uc.ncgFolder = action.folder
            uc.saveConfig(false)
            true to action.folder
        }
    }

    suspend fun changePhoto(slot: Int, path: String, replace: Boolean): Pair<Boolean, String> {
        // scale + re-encode so any picked image fits Telegram's profile photo limits
        val prepared = try {
            val bmp = android.graphics.BitmapFactory.decodeFile(path) ?: return false to "не удалось прочитать файл"
            val side = 800
            val scale = minOf(1f, side.toFloat() / maxOf(bmp.width, bmp.height))
            val scaled = if (scale < 1f) android.graphics.Bitmap.createScaledBitmap(bmp, (bmp.width * scale).toInt(), (bmp.height * scale).toInt(), true) else bmp
            val out = java.io.File(ApplicationLoader.applicationContext.cacheDir, "ggtg_photo_${System.nanoTime()}.jpg")
            out.outputStream().use { scaled.compress(android.graphics.Bitmap.CompressFormat.JPEG, 88, it) }
            out
        } catch (t: Throwable) { return false to "не удалось подготовить фото" }
        try {
            val user = MessagesController.getInstance(slot).getInputUser(UserConfig.getInstance(slot).currentUser)
            var old: TLRPC.InputPhoto? = null
            if (replace) {
                val get = TLRPC.TL_photos_getUserPhotos().apply { user_id = user; limit = 1 }
                val r = GgtgAccounts.request(slot, get)?.first
                val first = (r as? TLRPC.photos_Photos)?.photos?.firstOrNull() as? TLRPC.TL_photo
                if (first != null) old = TLRPC.TL_inputPhoto().apply { id = first.id; access_hash = first.access_hash; file_reference = first.file_reference }
            }
            val file = withTimeoutOrNull(90_000L) {
                suspendCancellableCoroutine<TLRPC.InputFile?> { cont ->
                    FileLoader.getInstance(slot).uploadFile(prepared.absolutePath) { f -> if (cont.isActive) cont.resumeWith(Result.success(f)) }
                }
            } ?: return false to "ошибка загрузки фото"
            val req = TLRPC.TL_photos_uploadProfilePhoto().apply { flags = 1; this.file = file }
            val pair = GgtgAccounts.request(slot, req) ?: return false to "таймаут"
            if (pair.second != null) return false to pair.second!!.text
            if (old != null) {
                val del = TLRPC.TL_photos_deletePhotos().apply { id.add(old) }
                GgtgAccounts.request(slot, del)
            }
            val resPhoto = (pair.first as? TLRPC.TL_photos_photo)?.photo
            if (resPhoto is TLRPC.TL_photo) {
                val small = FileLoader.getClosestPhotoSizeWithSize(resPhoto.sizes, 150)
                val big = FileLoader.getClosestPhotoSizeWithSize(resPhoto.sizes, 800)
                val userPhoto = TLRPC.TL_userProfilePhoto().apply {
                    photo_id = resPhoto.id
                    photo_small = small?.location
                    photo_big = big?.location
                    dc_id = resPhoto.dc_id
                    has_video = resPhoto.video_sizes.isNotEmpty()
                }
                val uc = UserConfig.getInstance(slot)
                val curUser = uc.currentUser ?: MessagesController.getInstance(slot).getUser(uc.clientUserId)
                if (curUser != null) {
                    curUser.photo = userPhoto
                    uc.currentUser?.photo = userPhoto
                    MessagesController.getInstance(slot).putUser(curUser, false)
                    val users = ArrayList<TLRPC.User>()
                    users.add(curUser)
                    MessagesStorage.getInstance(slot).putUsersAndChats(users, null, false, true)
                    uc.saveConfig(true)
                }
            }
            AndroidUtilities.runOnUIThread {
                NotificationCenter.getInstance(slot).postNotificationName(NotificationCenter.mainUserInfoChanged)
                NotificationCenter.getInstance(slot).postNotificationName(NotificationCenter.updateInterfaces, MessagesController.UPDATE_MASK_ALL)
                NotificationCenter.getInstance(slot).postNotificationName(NotificationCenter.updateInterfaces, MessagesController.UPDATE_MASK_AVATAR)
            }
            return true to ""
        } finally {
            prepared.delete()
        }
    }

    suspend fun deletePhoto(slot: Int): Pair<Boolean, String> {
        val uc = UserConfig.getInstance(slot)
        val curUser = uc.currentUser ?: MessagesController.getInstance(slot).getUser(uc.clientUserId)
        val inputUser = MessagesController.getInstance(slot).getInputUser(curUser)
        try {
            val get = TLRPC.TL_photos_getUserPhotos().apply {
                user_id = inputUser
                offset = 0
                limit = 100
                max_id = 0
            }
            val pair = GgtgAccounts.request(slot, get)
            val photosRes = pair?.first as? TLRPC.photos_Photos
            val photosToDelete = ArrayList<TLRPC.InputPhoto>()
            if (photosRes != null) {
                for (p in photosRes.photos) {
                    if (p is TLRPC.TL_photo) {
                        photosToDelete.add(TLRPC.TL_inputPhoto().apply {
                            id = p.id
                            access_hash = p.access_hash
                            file_reference = p.file_reference
                        })
                    }
                }
            }
            if (photosToDelete.isNotEmpty()) {
                val del = TLRPC.TL_photos_deletePhotos().apply {
                    id.addAll(photosToDelete)
                }
                GgtgAccounts.request(slot, del)
            }

            val updateEmpty = TLRPC.TL_photos_updateProfilePhoto().apply {
                id = TLRPC.TL_inputPhotoEmpty()
            }
            GgtgAccounts.request(slot, updateEmpty)

            if (curUser != null) {
                curUser.photo = TLRPC.TL_userProfilePhotoEmpty()
                uc.currentUser?.photo = curUser.photo
                MessagesController.getInstance(slot).putUser(curUser, false)
                val users = ArrayList<TLRPC.User>()
                users.add(curUser)
                MessagesStorage.getInstance(slot).putUsersAndChats(users, null, false, true)
                uc.saveConfig(true)
            }
            AndroidUtilities.runOnUIThread {
                NotificationCenter.getInstance(slot).postNotificationName(NotificationCenter.mainUserInfoChanged)
                NotificationCenter.getInstance(slot).postNotificationName(NotificationCenter.updateInterfaces, MessagesController.UPDATE_MASK_ALL)
                NotificationCenter.getInstance(slot).postNotificationName(NotificationCenter.updateInterfaces, MessagesController.UPDATE_MASK_AVATAR)
            }
            return true to ""
        } catch (t: Throwable) {
            return false to (t.message ?: "ошибка удаления фото")
        }
    }

    suspend fun updateProfile(slot: Int, first: String?, last: String?, about: String?): Pair<Boolean, String> {
        val req = TL_account.updateProfile()
        if (first != null) { req.flags = req.flags or 1; req.first_name = first }
        if (last != null) { req.flags = req.flags or 2; req.last_name = last }
        if (about != null) { req.flags = req.flags or 4; req.about = about }
        val pair = GgtgAccounts.request(slot, req) ?: return false to "таймаут"
        if (pair.second != null) return false to pair.second!!.text
        val updatedUser = pair.first as? TLRPC.User
        val uc = UserConfig.getInstance(slot)
        val user = uc.currentUser ?: MessagesController.getInstance(slot).getUser(uc.clientUserId)
        if (user != null) {
            if (first != null) user.first_name = first
            if (last != null) user.last_name = last
            if (updatedUser != null) {
                user.first_name = updatedUser.first_name
                user.last_name = updatedUser.last_name
            }
            uc.currentUser?.first_name = user.first_name
            uc.currentUser?.last_name = user.last_name
            MessagesController.getInstance(slot).putUser(user, false)
            val users = ArrayList<TLRPC.User>()
            users.add(user)
            MessagesStorage.getInstance(slot).putUsersAndChats(users, null, false, true)
            uc.saveConfig(true)
        }
        AndroidUtilities.runOnUIThread {
            NotificationCenter.getInstance(slot).postNotificationName(NotificationCenter.mainUserInfoChanged)
            NotificationCenter.getInstance(slot).postNotificationName(NotificationCenter.updateInterfaces, MessagesController.UPDATE_MASK_NAME)
        }
        return true to ""
    }

    suspend fun updateUsername(slot: Int, username: String): Pair<Boolean, String> {
        val clean = username.trim().removePrefix("@")
        val req = TL_account.updateUsername().apply { this.username = clean }
        val pair = GgtgAccounts.request(slot, req) ?: return false to "таймаут"
        if (pair.second != null) return false to pair.second!!.text
        val updatedUser = pair.first as? TLRPC.User
        val uc = UserConfig.getInstance(slot)
        val user = uc.currentUser ?: MessagesController.getInstance(slot).getUser(uc.clientUserId)
        if (user != null) {
            user.username = clean
            if (updatedUser != null) {
                user.username = updatedUser.username
                user.usernames = updatedUser.usernames
            }
            uc.currentUser?.username = user.username
            MessagesController.getInstance(slot).putUser(user, false)
            val users = ArrayList<TLRPC.User>()
            users.add(user)
            MessagesStorage.getInstance(slot).putUsersAndChats(users, null, false, true)
            uc.saveConfig(true)
        }
        AndroidUtilities.runOnUIThread {
            NotificationCenter.getInstance(slot).postNotificationName(NotificationCenter.mainUserInfoChanged)
            NotificationCenter.getInstance(slot).postNotificationName(NotificationCenter.updateInterfaces, MessagesController.UPDATE_MASK_NAME)
        }
        return true to (if (clean.isEmpty()) "" else "@$clean")
    }

    private suspend fun resetAuthorizations(slot: Int): Pair<Boolean, String> {
        val pair = GgtgAccounts.request(slot, TLRPC.TL_auth_resetAuthorizations()) ?: return false to "таймаут"
        return if (pair.second == null) true to "" else false to pair.second!!.text
    }

    private fun firstAsciiLetter(text: String?, fallback: Char): Char {
        text?.forEach { if ((it in 'a'..'z') || (it in 'A'..'Z')) return it.lowercaseChar() }
        return fallback
    }
}
