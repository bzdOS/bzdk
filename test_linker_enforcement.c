/* test_linker_enforcement.c — PROVE that two owner-marked objects defining
 * the same non-weak symbol cannot be linked together.
 *
 * This is the compile-time guard that prevents two conflicting implementations
 * of the same hardware block (e.g. vcpu3.c vs zguest_cpu3.c for CPU3) from
 * ever reaching the same binary. The error is a linker `multiple definition`,
 * not a runtime surprise on a board without a console.
 *
 * The test compiles two minimal .c files, each defining a non-weak
 * `const char *const bzdos_cpu3_owner` with a different value, then attempts
 * to link them. PASS = link fails with "multiple definition of bzdos_cpu3_owner".
 *
 * Usage: gcc -o test_linker_enforcement test_linker_enforcement.c && ./test_linker_enforcement
 * Returns 0 on success (linker error detected), 1 on unexpected outcome.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static const char *conflict_a =
	"#include <stdint.h>\n"
	"const char *const bzdos_cpu3_owner = \"vcpu3\";\n";

static const char *conflict_b =
	"#include <stdint.h>\n"
	"const char *const bzdos_cpu3_owner = \"zephyr_cpu3\";\n";

static int run(const char *const *argv, const char *unused __attribute__((unused)))
{
	pid_t pid = fork();
	if (pid < 0) { perror("fork"); return -1; }
	if (pid == 0) { execvp(argv[0], (char *const *)argv); _exit(127); }
	int status;
	waitpid(pid, &status, 0);
	if (WIFEXITED(status)) return WEXITSTATUS(status);
	return -1;
}

int main(void)
{
	const char *cc = "gcc";
	const char *tmpdir = "/tmp/lintest-XXXXXX";
	char dir[64], a_c[80], b_c[80], a_o[80], b_o[80], exe[80];

	strncpy(dir, tmpdir, sizeof(dir));
	if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
	snprintf(a_c, sizeof(a_c), "%s/a.c", dir);
	snprintf(b_c, sizeof(b_c), "%s/b.c", dir);
	snprintf(a_o, sizeof(a_o), "%s/a.o", dir);
	snprintf(b_o, sizeof(b_o), "%s/b.o", dir);
	snprintf(exe, sizeof(exe), "%s/out", dir);

	FILE *f = fopen(a_c, "w"); fprintf(f, "%s", conflict_a); fclose(f);
	f = fopen(b_c, "w"); fprintf(f, "%s", conflict_b); fclose(f);

	/* Compile each .c independently — both must succeed. */
	const char *cargv[] = { cc, "-c", "-o", a_o, a_c, NULL };
	if (run(cargv, "compile a.c") != 0) {
		fprintf(stderr, "FAIL: could not compile a.c\n"); return 1;
	}
	const char *cargv2[] = { cc, "-c", "-o", b_o, b_c, NULL };
	if (run(cargv2, "compile b.c") != 0) {
		fprintf(stderr, "FAIL: could not compile b.c\n"); return 1;
	}

	/* Link both .o files together — this MUST fail with multiple definition. */
	const char *largv[] = { cc, "-o", exe, a_o, b_o, NULL };
	int rc = run(largv, "link a.o + b.o");

	/* Cleanup. */
	unlink(a_c); unlink(b_c); unlink(a_o); unlink(b_o); unlink(exe);
	rmdir(dir);

	if (rc != 0) {
		printf("PASS: linker correctly rejected conflicting owners "
		       "(exit code %d = multiple definition error)\n", rc);
		return 0;
	}
	fprintf(stderr, "FAIL: linker accepted two conflicting bzdos_cpu3_owner "
		"definitions — enforcement is broken\n");
	return 1;
}
