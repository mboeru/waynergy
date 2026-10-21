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
		helper, "c", clipMonitorAddr.sun_path, NULL};
	char *primary_text[] = {"wl-paste", "-n", "--type", "text", "--primary", "-w",
		helper, "p", clipMonitorAddr.sun_path, NULL};
	char *image[] = {"wl-paste", "-n", "--type", "image", "-w",
		helper, "c", clipMonitorAddr.sun_path, "bitmap", NULL};
	char *primary_image[] = {"wl-paste", "-n", "--type", "image", "--primary", "-w",
		helper, "p", clipMonitorAddr.sun_path, "bitmap", NULL};
	char **argv[] = {text, primary_text, image, primary_image};
	for (int i = 0; i < CLIP_MONITOR_COUNT; ++i) {
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

static uint16_t clipReadLe16(const unsigned char *data)
{
	return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t clipReadLe32(const unsigned char *data)
{
	return (uint32_t)data[0] |
	       ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16) |
	       ((uint32_t)data[3] << 24);
}

static void clipWriteLe32(unsigned char *data, uint32_t value)
{
	data[0] = value & 0xff;
	data[1] = (value >> 8) & 0xff;
	data[2] = (value >> 16) & 0xff;
	data[3] = value >> 24;
}

/*
 * Synergy's bitmap wire format is a Windows DIB: a 40-byte
 * BITMAPINFOHEADER followed immediately by 24- or 32-bit BI_RGB pixels. It
 * is a BMP file with the 14-byte BITMAPFILEHEADER omitted. ImageMagick
 * normalizes that BMP to PNG so Wayland applications get the MIME type they
 * most consistently support for pasted images.
 */
bool clipWlCopyBitmap(enum uSynergyClipboardId id, const unsigned char *data, size_t len)
{
	static const size_t bmp_header_len = 14;
	unsigned char bmp_header[14] = {0};
	unsigned char normalized_dib_header[40];
	uint32_t dib_size, compression;
	int32_t width, height;
	uint16_t planes, bits_per_pixel;
	uint64_t abs_height, row_bits, row_bytes, pixel_bytes;
	bool normalize_truncated_v5 = false;
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
	if (len < 40 || len > UINT32_MAX - bmp_header_len) {
		logWarn("Rejecting Synergy bitmap with invalid size: %zu bytes", len);
		return false;
	}

	dib_size = clipReadLe32(data);
	width = (int32_t)clipReadLe32(data + 4);
	height = (int32_t)clipReadLe32(data + 8);
	planes = clipReadLe16(data + 12);
	bits_per_pixel = clipReadLe16(data + 14);
	compression = clipReadLe32(data + 16);

	if ((dib_size != 40 && dib_size != 108 && dib_size != 124) || dib_size > len ||
	    width <= 0 || height == 0 || height == INT32_MIN || planes != 1 ||
	    (bits_per_pixel != 24 && bits_per_pixel != 32) ||
	    (compression != 0 && compression != 3)) {
		logWarn("Rejecting unsupported Synergy bitmap: header=%u width=%d height=%d planes=%u bpp=%u compression=%u",
		        dib_size, width, height, planes, bits_per_pixel, compression);
		return false;
	}
	abs_height = height < 0 ? (uint64_t)(-(int64_t)height) : (uint64_t)height;
	row_bits = (uint64_t)(uint32_t)width * bits_per_pixel;
	row_bytes = ((row_bits + 31) / 32) * 4;
	pixel_bytes = row_bytes * abs_height;
	if (pixel_bytes > SIZE_MAX - 40 || len < 40 + (size_t)pixel_bytes) {
		logWarn("Rejecting truncated Synergy bitmap: %zu bytes for %dx%d@%u",
		        len, width, height, bits_per_pixel);
		return false;
	}

	haveRemoteBitmap[id] = clipBitmapHash(data, len, &remoteBitmapHash[id]);

	/* macOS can provide a BITMAPV5HEADER, but Synergy's wire converter keeps
	 * only the first 40 bytes while leaving biSize set to 124. In that case
	 * the pixel array starts at byte 40. Normalize it to a BI_RGB info header;
	 * the received 32-bit pixels are already in the required BGRA order. */
	if (dib_size > 40 && len < dib_size + pixel_bytes) {
		memcpy(normalized_dib_header, data, sizeof(normalized_dib_header));
		clipWriteLe32(normalized_dib_header, 40);
		clipWriteLe32(normalized_dib_header + 16, 0);
		dib_size = 40;
		compression = 0;
		normalize_truncated_v5 = true;
	}

	bmp_header[0] = 'B';
	bmp_header[1] = 'M';
	clipWriteLe32(bmp_header + 2, (uint32_t)(bmp_header_len + len));
	clipWriteLe32(bmp_header + 10, (uint32_t)(bmp_header_len + dib_size));

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

	if (!write_full(bmp_pipe[1], bmp_header, sizeof(bmp_header), 0) ||
	    (normalize_truncated_v5 &&
	     (!write_full(bmp_pipe[1], normalized_dib_header, sizeof(normalized_dib_header), 0) ||
	      !write_full(bmp_pipe[1], data + 40, len - 40, 0))) ||
	    (!normalize_truncated_v5 && !write_full(bmp_pipe[1], data, len, 0))) {
		logPErr("writing bitmap to ImageMagick");
		goto done;
	}
	logInfo("Placed remote %dx%d image on Wayland clipboard", width, height < 0 ? -height : height);
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
	return ok;
}
