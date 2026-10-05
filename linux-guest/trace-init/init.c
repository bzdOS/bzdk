/* SPDX-License-Identifier: BSD-2-Clause */
/* init.c — initramfs /init for the register-trace rig: decode one MPEG-2
 * I-frame through the kernel's V4L2 stateless interface on the A64 video
 * engine, then idle.  No libc: raw syscalls.  Input is embedded (i64.h). */
#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/videodev2.h>
#include <linux/media.h>
#include <linux/v4l2-controls.h>
#include "i64.h"

static long sc(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
	return x0;
}
#define S3(n,a,b,c) sc(n,(long)(a),(long)(b),(long)(c),0,0,0)
static int openf(const char *p, int fl) { return (int)sc(56, -100, (long)p, fl, 0, 0, 0); }
static long ioc(int fd, unsigned long r, void *a) { return S3(29, fd, r, a); }
static int kfd = 1;
static void out(const char *s) { long n = 0; while (s[n]) n++; S3(64, kfd, s, n); }
static void hex(unsigned long v)
{
	char b[20]; int i;
	b[0] = '0'; b[1] = 'x';
	for (i = 0; i < 16; i++) b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
	b[18] = ' '; b[19] = 0;
	out(b);
}
static void say(const char *s, long v) { out(s); hex((unsigned long)v); out("\n"); }
static void zero(void *p, long n) { char *c = p; while (n--) *c++ = 0; }

static const __u8 zz_intra[64] = {
	8,16,16,19,16,19,22,22,22,22,22,22,26,24,26,27,27,27,26,26,26,26,27,27,27,29,29,29,34,34,34,29,
	29,29,27,27,29,29,32,32,34,34,37,38,37,35,35,34,35,38,38,40,40,40,48,48,46,46,56,56,58,69,69,83 };

void __attribute__((noreturn)) idle(void)
{
	for (;;) { struct { long s, n; } ts = { 100, 0 }; S3(101, &ts, 0, 0); }
}

void __attribute__((noreturn)) fail(const char *what, long rc)
{
	say(what, rc);
	out("TRACE-DONE\n");
	idle();
}

void __attribute__((noreturn)) _start_c(void)
{
	int vfd, mfd, rfd, i;
	struct v4l2_capability cap;
	struct v4l2_format f;
	struct v4l2_requestbuffers rb;
	struct v4l2_buffer b;
		struct v4l2_exportbuffer eb;
	struct media_request_alloc_dummy { int fd; } ra;
	struct v4l2_ctrl_mpeg2_sequence seq;
	struct v4l2_ctrl_mpeg2_picture pic;
	struct v4l2_ctrl_mpeg2_quantisation q;
	struct v4l2_ext_control ec[3];
	struct v4l2_ext_controls ecs;
	void *obuf, *cbuf;
	__u8 *p;
	long r, nz; int iter;
	struct { int fd; short ev, rev; } pfd;
	struct { long s, n; } ts;
	enum v4l2_buf_type ot = V4L2_BUF_TYPE_VIDEO_OUTPUT, ct = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	sc(40, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0, 0);
	{ int k = openf("/dev/kmsg", 1); if (k >= 0) kfd = k; }
	out("\ninit: A64 VE trace rig\n");
	sc(101, (long)&(struct { long s, n; }){ 1, 0 }, 0, 0, 0, 0, 0);

	vfd = openf("/dev/video0", 2);
	mfd = openf("/dev/media0", 2);
	say("video0 fd", vfd); say("media0 fd", mfd);
	if (vfd < 0 || mfd < 0) fail("no device", vfd < 0 ? vfd : mfd);
	zero(&cap, sizeof cap);
	r = ioc(vfd, VIDIOC_QUERYCAP, &cap); say("querycap", r); say("caps", cap.device_caps);

	zero(&f, sizeof f);
	f.type = ot;
	f.fmt.pix.width = 64; f.fmt.pix.height = 64; f.fmt.pix.field = V4L2_FIELD_NONE;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_MPEG2_SLICE;
	f.fmt.pix.sizeimage = 0x20000;
	r = ioc(vfd, VIDIOC_S_FMT, &f); say("s_fmt out", r);
	zero(&f, sizeof f); f.type = ct;
	r = ioc(vfd, VIDIOC_G_FMT, &f); say("g_fmt cap", r);
	say(" cap fourcc", f.fmt.pix.pixelformat); say(" cap w", f.fmt.pix.width);
	say(" cap size", f.fmt.pix.sizeimage);
	r = ioc(vfd, VIDIOC_S_FMT, &f); say("s_fmt cap", r);

	zero(&rb, sizeof rb); rb.type = ot; rb.count = 1; rb.memory = V4L2_MEMORY_MMAP;
	r = ioc(vfd, VIDIOC_REQBUFS, &rb); say("reqbufs out", r);
	zero(&rb, sizeof rb); rb.type = ct; rb.count = 1; rb.memory = V4L2_MEMORY_MMAP;
	r = ioc(vfd, VIDIOC_REQBUFS, &rb); say("reqbufs cap", r);

	zero(&b, sizeof b);
	b.type = ot; b.memory = V4L2_MEMORY_MMAP; b.index = 0;
	r = ioc(vfd, VIDIOC_QUERYBUF, &b); say("querybuf out", r);
	obuf = (void *)sc(222, 0, b.length, 3, 1, vfd, b.m.offset);
	say("mmap out", (long)obuf);
	if ((unsigned long)obuf > -4096UL) fail("mmap out failed", (long)obuf);
	p = obuf; for (i = 0; i < I64_LEN; i++) p[i] = i64_data[i];

	zero(&b, sizeof b);
	b.type = ct; b.memory = V4L2_MEMORY_MMAP; b.index = 0;
	r = ioc(vfd, VIDIOC_QUERYBUF, &b); say("querybuf cap", r);
	cbuf = (void *)sc(222, 0, b.length, 3, 1, vfd, b.m.offset);
	say("mmap cap", (long)cbuf);
	say("cap len", b.length);

	zero(&seq, sizeof seq); zero(&pic, sizeof pic); zero(&q, sizeof q);
	seq.horizontal_size = 64; seq.vertical_size = 64; seq.vbv_buffer_size = 112 * 2048 / 8;
	seq.profile_and_level_indication = 0x48; seq.chroma_format = 1;
	seq.flags = V4L2_MPEG2_SEQ_FLAG_PROGRESSIVE;
	pic.picture_coding_type = V4L2_MPEG2_PIC_CODING_TYPE_I;
	pic.f_code[0][0] = pic.f_code[0][1] = pic.f_code[1][0] = pic.f_code[1][1] = 15;
	pic.flags = V4L2_MPEG2_PIC_FLAG_FRAME_PRED_DCT | V4L2_MPEG2_PIC_FLAG_PROGRESSIVE;
	pic.picture_structure = V4L2_MPEG2_PIC_FRAME;
	for (i = 0; i < 64; i++) { q.intra_quantiser_matrix[i] = zz_intra[i]; q.non_intra_quantiser_matrix[i] = 16;
		q.chroma_intra_quantiser_matrix[i] = zz_intra[i]; q.chroma_non_intra_quantiser_matrix[i] = 16; }
	zero(ec, sizeof ec);
	ec[0].id = V4L2_CID_STATELESS_MPEG2_SEQUENCE; ec[0].ptr = &seq; ec[0].size = sizeof seq;
	ec[1].id = V4L2_CID_STATELESS_MPEG2_PICTURE; ec[1].ptr = &pic; ec[1].size = sizeof pic;
	ec[2].id = V4L2_CID_STATELESS_MPEG2_QUANTISATION; ec[2].ptr = &q; ec[2].size = sizeof q;
	zero(&ecs, sizeof ecs);
	ecs.which = V4L2_CTRL_WHICH_REQUEST_VAL; ecs.request_fd = rfd; ecs.count = 3; ecs.controls = ec;
	p = cbuf;

	for (iter = 0; iter < 3; iter++) {
		long fl, nzc;
		r = ioc(mfd, MEDIA_IOC_REQUEST_ALLOC, &ra);
		rfd = ra.fd;
		ecs.request_fd = rfd;
		r = ioc(vfd, VIDIOC_S_EXT_CTRLS, &ecs);
		zero(&b, sizeof b);
		b.type = ct; b.memory = V4L2_MEMORY_MMAP; b.index = 0;
		r = ioc(vfd, VIDIOC_QBUF, &b);
		zero(&b, sizeof b);
		b.type = ot; b.memory = V4L2_MEMORY_MMAP; b.index = 0;
		b.bytesused = I64_LEN;
		b.flags = V4L2_BUF_FLAG_REQUEST_FD; b.request_fd = rfd;
		r = ioc(vfd, VIDIOC_QBUF, &b);
		if (iter == 0) {
			ioc(vfd, VIDIOC_STREAMON, &ot);
			ioc(vfd, VIDIOC_STREAMON, &ct);
		}
		zero(p, 6144);
		r = ioc(rfd, MEDIA_REQUEST_IOC_QUEUE, 0);
		pfd.fd = rfd; pfd.ev = 0x002; pfd.rev = 0;
		ts.s = 6; ts.n = 0;
		sc(73, (long)&pfd, 1, (long)&ts, 0, 8, 0);
		zero(&b, sizeof b);
		b.type = ct; b.memory = V4L2_MEMORY_MMAP;
		r = ioc(vfd, VIDIOC_DQBUF, &b);
		fl = b.flags;
		zero(&b, sizeof b);
		b.type = ot; b.memory = V4L2_MEMORY_MMAP;
		ioc(vfd, VIDIOC_DQBUF, &b);
		sc(57, rfd, 0, 0, 0, 0, 0);
		p = cbuf; nzc = 0;
		for (i = 0; i < 6144; i++) if (p[i]) nzc++;
		out("ITER"); hex(iter); hex(fl); hex(nzc); out("\n");
		if (iter == 2) {
			static const char hx[] = "0123456789abcdef";
			char ln[2 + 128 + 1];
			int row, c;
			for (row = 0; row < 96; row++) {
				ln[0] = 'D'; ln[1] = (char)('A' + (row >> 4));
				for (c = 0; c < 64; c++) {
					unsigned v = p[row * 64 + c];
					ln[2 + 2 * c] = hx[v >> 4]; ln[3 + 2 * c] = hx[v & 15];
				}
				ln[130] = '\n'; ln[131] = 0;
				out(ln);
			}
		}
	}
	out("TRACE-DONE\n");
	idle();
}

__asm__(".globl _start\n_start:\n mov x0, sp\n and sp, x0, #-16\n bl _start_c\n");
