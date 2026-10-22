#include <assert.h>
#include "clip-bitmap.h"
/* Include the parser so screen transitions exercise the actual send path. */
#include "../src/uSynergy.c"

static unsigned grabs, starts, ends, chunks;
static unsigned char received[2 * 1024 * 1024];
static size_t received_len, expected_len;
static uSynergyContext *peer_context;
static unsigned incoming_callbacks;

void Exit(enum sigExitStatus status) { abort(); }

static uint32_t be32(const unsigned char *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static bool discard_reply(uSynergyCookie cookie, const unsigned char *data, int len)
{
	return true;
}

static void receive_clipboard(uSynergyCookie cookie, enum uSynergyClipboardId id,
	uint32_t format, const unsigned char *data, uint32_t len)
{
	assert(id == 0 && format == be32(received + 4));
	assert(len == be32(received + 8));
	assert(!memcmp(data, received + 12, len));
	++incoming_callbacks;
}

static bool send_packet(uSynergyCookie cookie, const unsigned char *data, int len)
{
	assert(be32(data) == (unsigned)len - 4);
	if (len == 8 && !memcmp(data + 4, "CNOP", 4))
		return true;
	assert(be32(data + 9) == 42);
	if (!memcmp(data + 4, "CCLP", 4)) {
		++grabs;
		return true;
	}
	assert(!memcmp(data + 4, "DCLP", 4));
	assert(be32(data + 14) == (unsigned)len - 18);
	if (data[13] == SYN_DATA_START) {
		char size[32] = {0};
		assert((unsigned)len - 18 < sizeof(size));
		memcpy(size, data + 18, len - 18);
		expected_len = strtoul(size, NULL, 10);
		received_len = 0;
		++starts;
	} else if (data[13] == SYN_DATA_CHUNK) {
		assert(received_len + len - 18 <= sizeof(received));
		memcpy(received + received_len, data + 18, len - 18);
		received_len += len - 18;
		++chunks;
	} else {
		assert(data[13] == SYN_DATA_END && expected_len == received_len);
		++ends;
	}
	/* Feed each streamed packet back through the real receive parser. */
	struct sspBuf incoming = {.data = data + 4, .len = len - 4};
	sProcessMessage(peer_context, &incoming);
	return true;
}

static void hash_tests(void)
{
	/* Two rows, one pixel each: bottom-up BGR24 vs top-down BGRA32. */
	unsigned char a[48] = {40,0,0,0, 1,0,0,0, 2,0,0,0, 1,0,24,0};
	unsigned char b[48] = {40,0,0,0, 1,0,0,0, 254,255,255,255, 1,0,32,0};
	memcpy(a + 40, (unsigned char[]){1,2,3,99, 4,5,6,88}, 8);
	memcpy(b + 40, (unsigned char[]){4,5,6,255, 1,2,3,255}, 8);
	uint64_t ah, bh;
	assert(clipBitmapHash(a, sizeof(a), &ah));
	assert(clipBitmapHash(b, sizeof(b), &bh) && ah == bh);
	b[40]++;
	assert(clipBitmapHash(b, sizeof(b), &bh) && ah != bh);
	b[40]--;
	b[0] = 124; b[16] = 3; /* Exact legacy V5 header + BGRA payload. */
	assert(clipBitmapHash(b, sizeof(b), &bh) && ah == bh);
	assert(!clipBitmapHash(b, sizeof(b) - 1, &bh));
	b[0] = 108; /* Do not repair arbitrary truncated extended headers. */
	assert(!clipBitmapHash(b, sizeof(b), &bh));
	a[4] = 255; a[7] = 127;
	assert(!clipBitmapHash(a, sizeof(a), &bh));
}

static unsigned limit_grabs;
static bool limit_packet(uSynergyCookie cookie, const unsigned char *data, int len)
{
	if (len >= 8 && !memcmp(data + 4, "CCLP", 4))
		++limit_grabs;
	return true;
}

static void set_clipboard_option(uSynergyContext *context, uint32_t option, uint32_t value)
{
	unsigned char packet[16] = {'D','S','O','P'};
	buf_add_int32(packet + 4, 2);
	buf_add_int32(packet + 8, option);
	buf_add_int32(packet + 12, value);
	struct sspBuf message = {.data = packet, .len = sizeof(packet)};
	sProcessMessage(context, &message);
}

static void server_limit_tests(void)
{
	uSynergyContext *context = calloc(1, sizeof(*context));
	uSynergyInit(context);
	context->m_connected = context->m_hasReceivedHello = context->m_isCaptured = true;
	context->m_sendFunc = limit_packet;
	size_t len = 6 * 1024 * 1024;
	unsigned char *image = calloc(1, len);
	set_clipboard_option(context, USYNERGY_OPTION_CLIPBOARD_SIZE, 3072);
	assert(context->m_clipboardMaxBytes == 3 * 1024 * 1024);
	/* The failing real-world case: a PNG can be small on disk, while its
	 * marshalled bitmap exceeds the Mac server's 3 MiB limit. Do not grab
	 * (and clear) the remote clipboard for an image the server will reject. */
	assert(!uSynergyUpdateClipBufFormat(context, 0, 2, len, image));
	assert(!limit_grabs && !context->m_clipBuf[0]);
	assert(uSynergyUpdateClipBufFormat(context, 0, 2, 1024, image));
	assert(limit_grabs == 1);
	set_clipboard_option(context, USYNERGY_OPTION_CLIPBOARD_SIZE, 32768);
	assert(uSynergyUpdateClipBufFormat(context, 0, 2, len, image));
	assert(limit_grabs == 2); /* The previously rejected image can be retried. */
	/* Recheck limits if they change while an image waits for COUT. */
	set_clipboard_option(context, USYNERGY_OPTION_CLIPBOARD_SIZE, 3072);
	struct sspBuf leave = {.data = (const unsigned char *)"COUT", .len = 4};
	sProcessMessage(context, &leave);
	assert(!context->m_clipGrabbed[0] && !context->m_clipPos[0]);
	set_clipboard_option(context, USYNERGY_OPTION_CLIPBOARD_SHARING, 0);
	assert(!uSynergyUpdateClipBufFormat(context, 0, 0, 4, image));
	set_clipboard_option(context, USYNERGY_OPTION_CLIPBOARD_SHARING, 1);
	set_clipboard_option(context, USYNERGY_OPTION_CLIPBOARD_SIZE, 0);
	assert(!uSynergyUpdateClipBufFormat(context, 0, 0, 4, image));
	set_clipboard_option(context, USYNERGY_OPTION_CLIPBOARD_SIZE, UINT32_MAX);
	assert(!context->m_clipboardMaxBytes); /* Ignore negative option values. */
	struct sspBuf reset = {.data = (const unsigned char *)"CROP", .len = 4};
	sProcessMessage(context, &reset);
	assert(context->m_clipboardSharingEnabled &&
		context->m_clipboardMaxBytes == USYNERGY_MAX_CLIPBOARD_SIZE);
	unsigned char malformed[] = {'D','S','O','P',0,0,0,2, 'C','L','S','Z'};
	struct sspBuf bad = {.data = malformed, .len = sizeof(malformed)};
	sProcessMessage(context, &bad);
	assert(context->m_clipboardMaxBytes == USYNERGY_MAX_CLIPBOARD_SIZE);
	context->m_isCaptured = true;
	context->m_clipboardMaxBytes = 16;
	assert(uSynergyUpdateClipBufFormat(context, 0, 0, 4, image));
	assert(!uSynergyUpdateClipBufFormat(context, 0, 0, 5, image));
	free(context->m_clipBuf[0]);
	free(context);
	free(image);
}

int main(void)
{
	hash_tests();
	server_limit_tests();
	uSynergyContext *context = calloc(1, sizeof(*context));
	uSynergyInit(context);
	peer_context = calloc(1, sizeof(*peer_context));
	uSynergyInit(peer_context);
	peer_context->m_sendFunc = discard_reply;
	peer_context->m_clipboardCallback = receive_clipboard;
	context->m_connected = context->m_hasReceivedHello = context->m_isCaptured = true;
	context->m_sequenceNumber = 42;
	context->m_sendFunc = send_packet;
	size_t len = 1024 * 1024 + 17;
	unsigned char *image = malloc(len);
	for (size_t i = 0; i < len; ++i) image[i] = i * 17;
	uSynergyUpdateClipBufFormat(context, 0, 2, len, image);
	assert(grabs == 1 && starts == 0 && context->m_clipGrabbed[0]);
	uSynergyUpdateClipBufFormat(context, 0, 2, len, image);
	assert(grabs == 1); /* Exact image echo is suppressed. */
	struct sspBuf leave = {.data = (const unsigned char *)"COUT", .len = 4};
	sProcessMessage(context, &leave);
	assert(starts == 1 && ends == 1 && chunks > 1000);
	assert(incoming_callbacks == 1 && !peer_context->m_clipInStream[0]);
	assert(be32(received) == 1 && be32(received + 4) == 2 && be32(received + 8) == len);
	assert(!memcmp(received + 12, image, len));
	assert(!context->m_clipGrabbed[0]);
	/* An image finishing conversion after COUT is sent immediately. */
	image[0]++;
	uSynergyUpdateClipBufFormat(context, 0, 2, len, image);
	assert(grabs == 2 && starts == 2 && ends == 2);
	assert(!memcmp(received + 12, image, len));
	/* Text still uses format zero, and duplicate matching includes the format. */
	uSynergyUpdateClipBuf(context, 0, 5, "hello");
	assert(be32(received + 4) == 0 && !memcmp(received + 12, "hello", 5));
	uSynergyUpdateClipBufFormat(context, 0, 2, 5, (unsigned char *)"hello");
	assert(be32(received + 4) == 2);
	assert(incoming_callbacks == 4);
	unsigned before = grabs;
	uSynergyUpdateClipBufFormat(context, 2, 2, 5, image);
	uSynergyUpdateClipBufFormat(context, 0, 2, USYNERGY_MAX_CLIPBOARD_SIZE, image);
	assert(grabs == before);
	/* Find echoes in later formats, skip payloads safely, reject truncation. */
	unsigned char multi[] = {0,0,0,2, 0,0,0,0, 0,0,0,3, 'a','b','c',
		0,0,0,2, 0,0,0,3, 'd','e','f'};
	memcpy(context->m_clipBuf[0], multi, sizeof(multi));
	context->m_clipPos[0] = sizeof(multi);
	assert(uSynergyClipBufContains(context, 0, 2, 3, (unsigned char *)"def"));
	context->m_clipPos[0]--;
	assert(!uSynergyClipBufContains(context, 0, 2, 3, (unsigned char *)"def"));
	/* Reject an invalid clipboard ID and an over-limit advertised stream. */
	unsigned char bad_id[] = {'D','C','L','P', 2, 0,0,0,0, SYN_DATA_START, 0,0,0,1, '1'};
	struct sspBuf bad = {.data = bad_id, .len = sizeof(bad_id)};
	sProcessMessage(peer_context, &bad);
	unsigned char oversized[] = {'D','C','L','P', 0, 0,0,0,0, SYN_DATA_START,
		0,0,0,9, '1','3','4','2','1','7','7','2','9'};
	bad = (struct sspBuf){.data = oversized, .len = sizeof(oversized)};
	sProcessMessage(peer_context, &bad);
	assert(!peer_context->m_clipInStream[0] && incoming_callbacks == 4);
	free(peer_context->m_clipBuf[0]);
	free(peer_context);
	free(context->m_clipBuf[0]);
	free(context);
	free(image);
	puts("clipboard protocol and image echo tests passed");
}
