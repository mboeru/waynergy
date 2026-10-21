#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include "ssb.h"
#include "fdio_full.h"
#include "uSynergy.h"
#include "clip-bitmap.h"

extern char **environ;

static bool readBounded(struct ssb *s, FILE *input, size_t limit)
{
	unsigned char chunk[16384];
	size_t n;
	while ((n = fread(chunk, 1, sizeof(chunk), input))) {
		if (n > limit - s->pos ||
		    (s->size - s->pos < n && !ssb_grow_min(s, n)))
			return false;
		memcpy(s->buf + s->pos, chunk, n);
		s->pos += n;
	}
	return !ferror(input);
}

/* Drain stdout while the converter reads the watcher's stdin directly. Large
 * screenshots must not deadlock on write-then-read pipe capacity. */
static bool capture(char **args, struct ssb *s, size_t limit, int input_fd)
{
	int fd[2], status;
	pid_t pid;
	posix_spawn_file_actions_t actions;
	if (pipe(fd) < 0)
		return false;
	posix_spawn_file_actions_init(&actions);
	if (input_fd != STDIN_FILENO) {
		posix_spawn_file_actions_adddup2(&actions, input_fd, STDIN_FILENO);
		posix_spawn_file_actions_addclose(&actions, input_fd);
	}
	posix_spawn_file_actions_adddup2(&actions, fd[1], STDOUT_FILENO);
	posix_spawn_file_actions_addclose(&actions, fd[0]);
	posix_spawn_file_actions_addclose(&actions, fd[1]);
	int error = posix_spawnp(&pid, args[0], &actions, NULL, args, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(fd[1]);
	if (error) {
		close(fd[0]);
		return false;
	}
	FILE *output = fdopen(fd[0], "rb");
	bool ok = output && readBounded(s, output, limit);
	if (output)
		fclose(output);
	else
		close(fd[0]);
	if (!ok)
		kill(pid, SIGTERM);
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return false;
	}
	return ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* For mixed offers, the text watcher also attempts the image conversion.
 * Both watchers then deliver the same bitmap (deduplicated by the client),
 * or the original text remains available if image conversion fails. */
static bool offersImage(char cid)
{
	char *normal[] = {"wl-paste", "--list-types", NULL};
	char *primary[] = {"wl-paste", "--primary", "--list-types", NULL};
	struct ssb types = {0};
	bool found = false;
	if (capture(cid == 'p' ? primary : normal, &types, 65536, STDIN_FILENO)) {
		for (size_t i = 0; i + 6 <= types.pos; ++i)
			if ((i == 0 || types.buf[i - 1] == '\n') && !memcmp(types.buf + i, "image/", 6)) {
				found = true;
				break;
			}
	}
	ssb_free(&types);
	return found;
}

static bool convertBitmap(struct ssb *s, int input_fd)
{
	char *args[] = {"magick", "-[0]", "-alpha", "off", "-type", "TrueColor",
		"-depth", "8", "-compress", "none", "BMP3:-", NULL};
	struct clipBitmap bitmap;
	return capture(args, s, USYNERGY_MAX_CLIPBOARD_SIZE, input_fd) && s->pos >= 54 &&
		!memcmp(s->buf, "BM", 2) && clipBitmapLe32((unsigned char *)s->buf + 10) == 54 &&
		clipBitmapLe32((unsigned char *)s->buf + 14) == 40 &&
		clipBitmapParse((unsigned char *)s->buf + 14, s->pos - 14, &bitmap) &&
		bitmap.bits_per_pixel == 24;
}

static bool convertClipboardImage(char cid, struct ssb *output)
{
	int fd[2], status;
	pid_t producer;
	posix_spawn_file_actions_t actions;
	char *normal[] = {"wl-paste", "-n", "--type", "image", NULL};
	char *primary[] = {"wl-paste", "-n", "--primary", "--type", "image", NULL};
	if (pipe(fd) < 0)
		return false;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, fd[1], STDOUT_FILENO);
	posix_spawn_file_actions_addclose(&actions, fd[0]);
	posix_spawn_file_actions_addclose(&actions, fd[1]);
	int error = posix_spawnp(&producer, "wl-paste", &actions, NULL,
		cid == 'p' ? primary : normal, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(fd[1]);
	if (error) {
		close(fd[0]);
		return false;
	}
	bool ok = convertBitmap(output, fd[0]);
	close(fd[0]);
	if (!ok)
		kill(producer, SIGTERM);
	while (waitpid(producer, &status, 0) < 0) {
		if (errno != EINTR)
			return false;
	}
	return ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int main(int argc, char **argv)
{
	if ((argc != 3 && argc != 4) || strlen(argv[1]) != 1 ||
	    (argv[1][0] != 'c' && argv[1][0] != 'p') ||
	    (argc == 4 && strcmp(argv[3], "bitmap") && strcmp(argv[3], "text-only")))
		return EXIT_FAILURE;
	const char *state = getenv("CLIPBOARD_STATE");
	if (state && strcmp(state, "data"))
		return EXIT_SUCCESS;
	char cid = argv[1][0];
	bool bitmap = argc == 4 && !strcmp(argv[3], "bitmap");
	bool text_only = argc == 4 && !strcmp(argv[3], "text-only");
	struct ssb s = {0};
	struct sockaddr_un sa = {0};
	int sock = -1, result = EXIT_FAILURE;
	const unsigned char *data;
	size_t len;
	if (bitmap) {
		if (!convertBitmap(&s, STDIN_FILENO))
			goto done;
	} else {
		if (!readBounded(&s, stdin, USYNERGY_MAX_CLIPBOARD_SIZE - 12))
			goto done;
		if (!s.pos) {
			result = EXIT_SUCCESS;
			goto done;
		}
		if (!text_only && offersImage(cid)) {
			struct ssb converted = {0};
			if (convertClipboardImage(cid, &converted)) {
				ssb_free(&s);
				s = converted;
				bitmap = true;
			} else {
				ssb_free(&converted);
			}
		}
	}
	data = (unsigned char *)s.buf + (bitmap ? 14 : 0);
	len = s.pos - (bitmap ? 14 : 0);
	if (bitmap)
		cid = cid == 'c' ? 'C' : 'P';
	strncpy(sa.sun_path, argv[2], sizeof(sa.sun_path) - 1);
	sa.sun_family = AF_UNIX;
	if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0 ||
	    connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0)
		goto done;
	/* Upper-case clipboard IDs distinguish bitmap payloads while preserving
	 * the existing text helper's socket framing. */
	if (write_full(sock, &cid, 1, 0) && write_full(sock, &len, sizeof(len), 0) &&
	    write_full(sock, data, len, 0))
		result = EXIT_SUCCESS;
done:
	if (sock >= 0)
		close(sock);
	ssb_free(&s);
	return result;
}

