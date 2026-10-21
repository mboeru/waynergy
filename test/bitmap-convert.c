#include "clip-bitmap.h"
#include "ssb.h"
#include <string.h>

/* Exercise the shared parser/normalizer without a compositor or server. The
 * Python fixtures verify the resulting BMP through the real PNG converter. */
int main(int argc, char **argv)
{
	struct ssb input = {0};
	unsigned char *bmp = NULL;
	size_t len;
	int result = EXIT_FAILURE;
	if (!ssb_readfile(&input, stdin))
		goto done;
	if (argc == 2 && !strcmp(argv[1], "hash")) {
		uint64_t hash;
		if (clipBitmapHash((unsigned char *)input.buf, input.pos, &hash)) {
			printf("%016llx\n", (unsigned long long)hash);
			result = EXIT_SUCCESS;
		}
	} else if (argc == 1 && clipBitmapToBmp((unsigned char *)input.buf, input.pos, &bmp, &len)) {
		if (fwrite(bmp, 1, len, stdout) == len)
			result = EXIT_SUCCESS;
	}
done:
	free(bmp);
	ssb_free(&input);
	return result;
}
