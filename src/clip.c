#include "clip.h"
#include "wayland.h"
#include "net.h"
#include "config.h"
#include "clip-bitmap.h"

extern uSynergyContext synContext;
extern char **environ;

int clipMonitorFd;
struct sockaddr_un clipMonitorAddr;
pid_t clipMonitorPid[CLIP_MONITOR_COUNT];
static uint64_t remoteBitmapHash[2];
static bool haveRemoteBitmap[2];
static bool clipImagesEnabled;

static bool clipHaveImageConverter(void)
{
	int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
	if (null_fd < 0)
		return false;
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, null_fd, STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&actions, null_fd, STDERR_FILENO);
	char *args[] = {"magick", "-version", NULL};
	pid_t pid;
	int status = -1;
	sigWaitSIGCHLD(true);
	int error = posix_spawnp(&pid, "magick", &actions, NULL, args, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(null_fd);
	if (!error)
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
	sigWaitSIGCHLD(false);
	return !error && status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* check for wl-clipboard's presence */
bool clipHaveWlClipboard(void)
{
	bool ret = true;
	pid_t pid;
	int status;
	char *argv_0[] = {
		"wl-paste",
		"-v",
		NULL
	};
	char *argv_1[] = {
		"wl-copy",
		"-v",
		NULL
	};
	char **argv[] = {argv_0, argv_1};

	sigWaitSIGCHLD(true);

	for (int i = 0; i < 2; ++i) {
		if (posix_spawnp(&pid, argv[i][0], NULL, NULL, argv[i], environ)) {
			ret = false;
			goto done;
		}
		if (waitpid(pid, &status, 0) != pid) {
			ret = false;
			goto done;
		}
		if (status) {
			ret = false;
			goto done;
		}
		logDbg("Found %s", argv[i][0]);
	}
done:
	sigWaitSIGCHLD(false);
	return ret;
}

/* set up sockets */
bool clipSetupSockets()
{
	char *path;

	path = osGetRuntimePath("waynergy-clip-sock");
	strncpy(clipMonitorAddr.sun_path, path, sizeof(clipMonitorAddr.sun_path) - 1);
	free(path);
	unlink(clipMonitorAddr.sun_path);
	clipMonitorAddr.sun_family = AF_UNIX;
	if ((clipMonitorFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) == -1) {
		logPErr("socket");
		return false;
	}
	if (bind(clipMonitorFd, (struct sockaddr *)&clipMonitorAddr, sizeof(clipMonitorAddr)) == -1) {
		logPErr("bind");
		return false;
	}
	if (listen(clipMonitorFd, 8) == -1) {
		logPErr("listen");
		return false;
	}
	for (int i = POLLFD_CLIP_UPDATER; i < POLLFD_COUNT; ++i) {
		netPollFd[i].fd = -1;
	}
	return true;
}

/* Spawn separate text and image watchers for both selections. */
bool clipSpawnMonitors(void)
{
	clipImagesEnabled = configTryBool("clipboard/images", true);
	if (clipImagesEnabled && !clipHaveImageConverter()) {
		logWarn("ImageMagick unavailable; synchronizing text clipboards only");
		clipImagesEnabled = false;
	}
	/* Always use the companion helper from the same installation. */
	char *helper = "waynergy-clip-update";
#ifdef __linux__
	char helper_path[PATH_MAX];
	ssize_t pathlen = readlink("/proc/self/exe", helper_path, sizeof(helper_path) - 1);
	if (pathlen >= 0 && (size_t)pathlen < sizeof(helper_path) - 1) {
		helper_path[pathlen] = '\0';
		char *basename = strrchr(helper_path, '/');
		if (basename && sizeof(helper_path) - (basename + 1 - helper_path) >= sizeof("waynergy-clip-update")) {
			strcpy(basename + 1, "waynergy-clip-update");
			if (!access(helper_path, X_OK))
				helper = helper_path;
		}
	}
#endif
	char *text[] = {"wl-paste", "-n", "--type", "text", "-w",
		helper, "c", clipMonitorAddr.sun_path, clipImagesEnabled ? NULL : "text-only", NULL};
	char *primary_text[] = {"wl-paste", "-n", "--type", "text", "--primary", "-w",
		helper, "p", clipMonitorAddr.sun_path, clipImagesEnabled ? NULL : "text-only", NULL};
	char *image[] = {"wl-paste", "-n", "--type", "image", "-w",
		helper, "c", clipMonitorAddr.sun_path, "bitmap", NULL};
	char *primary_image[] = {"wl-paste", "-n", "--type", "image", "--primary", "-w",
		helper, "p", clipMonitorAddr.sun_path, "bitmap", NULL};
	char **argv[] = {text, primary_text, image, primary_image};
	for (int i = 0; i < (clipImagesEnabled ? CLIP_MONITOR_COUNT : 2); ++i) {
		int error = posix_spawnp(clipMonitorPid + i, "wl-paste", NULL, NULL, argv[i], environ);
		if (error) {
			errno = error;
			logPErr("spawn clipboard watcher");
			return false;
		}
	}
	return true;
}

/* process poll data */
void clipMonitorPollProc(struct pollfd *pfd)
{
	size_t len;
	char c_id, *buf = NULL;
	enum uSynergyClipboardId id;
	if (pfd->revents & POLLIN) {
		if (pfd->fd == clipMonitorFd) {
			for (int i = POLLFD_CLIP_UPDATER; i < POLLFD_COUNT; ++i) {
				if (netPollFd[i].fd == -1) {
					logDbg("Accepting");
					netPollFd[i].fd = accept4(clipMonitorFd, NULL, NULL, SOCK_CLOEXEC);
					if (netPollFd[i].fd == -1) {
						logPErr("accept");
						switch (errno) {
							case ECONNABORTED:
							case EINTR:
								break;
							default:
								logErr("clipboard update socket is broken");
								close(clipMonitorFd);
								clipMonitorFd = -1;
								netPollFd[POLLFD_CLIP_MON].fd = -1;
								break;
						}
					}
					return;
				}
			}
			logErr("No free updater file descriptors -- doing nothing");
		} else {
			if (!read_full(pfd->fd, &c_id, 1, 0)) {
				logPErr("Could not read clipboard ID");
				goto done;
			}
			if (!read_full(pfd->fd, &len, sizeof(len), 0)) {
				logPErr("Could not read clipboard data length");
				goto done;
			}
			if (len == 0)
				goto done;
			if (len > USYNERGY_MAX_CLIPBOARD_SIZE - 12) {
				logWarn("Rejecting oversized local clipboard: %zu bytes", len);
				goto done;
			}
			buf = xmalloc(len);
			if (!read_full(pfd->fd, buf, len, 0)) {
				logPErr("Could not read clipboard data");
				goto done;
			}
			if (c_id == 'p' || c_id == 'P') {
				id = SYNERGY_CLIPBOARD_SELECTION;
			} else if (c_id == 'c' || c_id == 'C') {
				id = SYNERGY_CLIPBOARD_CLIPBOARD;
			} else {
				logErr("Unknown clipboard ID %c", c_id);
				goto done;
			}
			logDbg("Clipboard data read for %c: %zd bytes", c_id, len);
			if (c_id == 'C' || c_id == 'P') {
				uint64_t hash;
				if (!clipBitmapHash((unsigned char *)buf, len, &hash)) {
					logWarn("Rejecting malformed local bitmap");
					goto done;
				}
				if (haveRemoteBitmap[id] && hash == remoteBitmapHash[id]) {
					logDbg("Ignoring image echoed from remote clipboard");
					goto done;
				}
				/* Barrier, Deskflow and modern Synergy use bitmap slot 2. */
				long format = configTryLong("clipboard/bitmap_format", 2);
				if (format != 1 && format != 2) {
					logWarn("Invalid clipboard/bitmap_format; using 2");
					format = 2;
				}
				haveRemoteBitmap[id] = false;
				uSynergyUpdateClipBufFormat(&synContext, id, format, len, (unsigned char *)buf);
				logInfo("Queued local image for remote clipboard: %zu bytes, format %ld", len, format);
			} else {
				haveRemoteBitmap[id] = false;
				uSynergyUpdateClipBuf(&synContext, id, len, buf);
			}
done:
			shutdown(pfd->fd, SHUT_RDWR);
			close(pfd->fd);
			pfd->fd = -1;
			free(buf);
		}
	}
	return;
}


/* set wayland clipboard with wl-copy */
bool clipWlCopy(enum uSynergyClipboardId id, const unsigned char *data, size_t len)
{
	pid_t pid;
	posix_spawn_file_actions_t fa;
	char *argv_0[] = {
			"wl-copy",
			"-f",
			NULL};

	char *argv_1[] = {
			"wl-copy",
			"-f",
			"--primary",
			NULL
		};
	char **argv[] = {argv_0, argv_1};
	/* create the pipe we will use to communicate with it */
	int fd[2];
	errno = 0;
	if (pipe(fd) == -1) {
		perror("pipe");
		return false;
	}
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, fd[0], STDIN_FILENO);
	posix_spawn_file_actions_addclose(&fa, fd[1]);
   	/* now we can spawn */
	errno = 0;
	if (posix_spawnp(&pid, "wl-copy", &fa, NULL, argv[id], environ)) {
		logPErr("wl-copy spawn");
		close(fd[0]);
		close(fd[1]);
		posix_spawn_file_actions_destroy(&fa);
		return false;
	}
	posix_spawn_file_actions_destroy(&fa);
	close(fd[0]);
	/* write to child process */
	write_full(fd[1], data, len, 0);
	close(fd[1]);
	haveRemoteBitmap[id] = false;
	return true;
}

/* Normalize all supported DIB layouts through the shared parser before
 * converting to the image/png offer expected by Wayland applications. */
bool clipWlCopyBitmap(enum uSynergyClipboardId id, const unsigned char *data, size_t len)
{
	unsigned char *bmp = NULL;
	size_t bmp_len;
	struct clipBitmap bitmap;
	int bmp_pipe[2] = {-1, -1};
	int png_pipe[2] = {-1, -1};
	posix_spawn_file_actions_t magick_actions, copy_actions;
	bool magick_actions_ready = false, copy_actions_ready = false;
	pid_t magick_pid, copy_pid;
	int spawn_error;
	bool ok = false;
	char *magick_argv[] = {"magick", "bmp:-", "png:-", NULL};
	char *copy_clipboard_argv[] = {"wl-copy", "-f", "--type", "image/png", NULL};
	char *copy_primary_argv[] = {"wl-copy", "-f", "--primary", "--type", "image/png", NULL};
	char **copy_argv[] = {copy_clipboard_argv, copy_primary_argv};

	if (id != SYNERGY_CLIPBOARD_CLIPBOARD && id != SYNERGY_CLIPBOARD_SELECTION) {
		logErr("Invalid clipboard ID for bitmap: %d", id);
		return false;
	}
	if (!clipImagesEnabled)
		return false;
	if (len > USYNERGY_MAX_CLIPBOARD_SIZE ||
	    !clipBitmapParse(data, len, &bitmap) || !clipBitmapToBmp(data, len, &bmp, &bmp_len)) {
		logWarn("Rejecting unsupported or malformed clipboard bitmap (%zu bytes)", len);
		return false;
	}

	if (pipe2(bmp_pipe, O_CLOEXEC) == -1 || pipe2(png_pipe, O_CLOEXEC) == -1) {
		logPErr("image clipboard pipe");
		goto done;
	}

	if (posix_spawn_file_actions_init(&magick_actions))
		goto done;
	magick_actions_ready = true;
	posix_spawn_file_actions_adddup2(&magick_actions, bmp_pipe[0], STDIN_FILENO);
	posix_spawn_file_actions_adddup2(&magick_actions, png_pipe[1], STDOUT_FILENO);
	spawn_error = posix_spawnp(&magick_pid, "magick", &magick_actions, NULL, magick_argv, environ);
	if (spawn_error) {
		errno = spawn_error;
		logPErr("ImageMagick spawn");
		goto done;
	}

	if (posix_spawn_file_actions_init(&copy_actions))
		goto done;
	copy_actions_ready = true;
	posix_spawn_file_actions_adddup2(&copy_actions, png_pipe[0], STDIN_FILENO);
	spawn_error = posix_spawnp(&copy_pid, "wl-copy", &copy_actions, NULL, copy_argv[id], environ);
	if (spawn_error) {
		errno = spawn_error;
		logPErr("image wl-copy spawn");
		goto done;
	}

	close(bmp_pipe[0]);
	bmp_pipe[0] = -1;
	close(png_pipe[0]);
	png_pipe[0] = -1;
	close(png_pipe[1]);
	png_pipe[1] = -1;

	if (!write_full(bmp_pipe[1], bmp, bmp_len, 0)) {
		logPErr("writing bitmap to ImageMagick");
		goto done;
	}
	haveRemoteBitmap[id] = clipBitmapHash(data, len, &remoteBitmapHash[id]);
	logInfo("Queued remote %ux%u image on Wayland clipboard%s", bitmap.width, bitmap.height,
		bitmap.repaired_legacy_header ? " (normalized legacy bitmap header)" : "");
	ok = true;

done:
	for (size_t i = 0; i < 2; ++i) {
		if (bmp_pipe[i] != -1)
			close(bmp_pipe[i]);
		if (png_pipe[i] != -1)
			close(png_pipe[i]);
	}
	if (magick_actions_ready)
		posix_spawn_file_actions_destroy(&magick_actions);
	if (copy_actions_ready)
		posix_spawn_file_actions_destroy(&copy_actions);
	free(bmp);
	return ok;
}
