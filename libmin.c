/* libmin.c — minimal freestanding mem* the compiler may emit implicitly
 * (e.g. struct copies -> memcpy) in a -nostdlib build. Not a full libc. */
#include <stdint.h>
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	while (n--)
		*d++ = *s++;
	return dst;
}

void *memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	while (n--)
		*d++ = (uint8_t)c;
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	if (d < s || d >= s + n) {
		while (n--)
			*d++ = *s++;
	} else {
		d += n;
		s += n;
		while (n--)
			*--d = *--s;
	}
	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;
	while (n--) {
		if (*x != *y)
			return (int)*x - (int)*y;
		x++; y++;
	}
	return 0;
}

/* libgcc replacement: __builtin_popcountll lowers to this in freestanding
 * builds (no libgcc linked). Simple, correct bit-count. */
int __popcountdi2(unsigned long long x)
{
	int n = 0;
	while (x) { x &= x - 1; n++; }
	return n;
}

/* __popcountsi2 too, in case a 32-bit popcount is emitted. */
int __popcountsi2(unsigned int x)
{
	int n = 0;
	while (x) { x &= x - 1; n++; }
	return n;
}
