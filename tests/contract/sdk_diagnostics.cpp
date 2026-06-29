#include "ubse_ssu_sdk.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef const ubse_ssu_sdk_ops_t *(*sdk_entry_fn)(void);
typedef const char *(*sdk_last_error_fn)(void);

static int expect_contains(const char *text, const char *needle)
{
    if (text == NULL || strstr(text, needle) == NULL) {
        fprintf(stderr, "expected text to contain [%s], got [%s]\n",
                needle, text == NULL ? "(null)" : text);
        return 1;
    }

    return 0;
}

static int read_file(const char *path, char *buf, size_t n)
{
    FILE *fp;
    size_t got;

    if (buf == NULL || n == 0) {
        return 0;
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        perror("fopen");
        return 0;
    }

    got = fread(buf, 1, n - 1, fp);
    buf[got] = '\0';
    fclose(fp);
    return 1;
}

template <typename Fn>
static Fn load_symbol(void *handle, const char *name)
{
    dlerror();
    void *sym = dlsym(handle, name);
    const char *err = dlerror();

    if (err != NULL) {
        fprintf(stderr, "missing symbol %s: %s\n", name, err);
        return NULL;
    }

    return reinterpret_cast<Fn>(sym);
}

static int expect_missing_manager_detail(const ubse_ssu_sdk_ops_t *sdk,
                                         sdk_last_error_fn last_error,
                                         const char *root)
{
    char socket_path[256];
    char log_path[256];
    char log_text[2048];
    ssu_resource_info_t resources[1];
    uint32_t count = 1;
    ssu_err_t err;

    snprintf(socket_path, sizeof(socket_path), "%s/missing-manager.fifo",
             root);
    snprintf(log_path, sizeof(log_path), "%s/sdk.log", root);
    unlink(log_path);

    setenv("SSU_MGR_SOCKET", socket_path, 1);
    setenv("UBSE_SSU_SDK_LOG", log_path, 1);

    err = sdk->init(NULL);
    if (err != SSU_OK) {
        fprintf(stderr, "sdk init failed: %d\n", err);
        return 1;
    }

    memset(resources, 0, sizeof(resources));
    err = sdk->list(resources, &count);
    if (err != SSU_ERR_NOT_FOUND) {
        fprintf(stderr, "expected list to return SSU_ERR_NOT_FOUND, got %d\n",
                err);
        sdk->fini();
        return 1;
    }

    if (expect_contains(last_error(), "open manager fifo failed") != 0 ||
        expect_contains(last_error(), socket_path) != 0 ||
        expect_contains(last_error(), "errno=2") != 0 ||
        expect_contains(last_error(), "hint=") != 0) {
        sdk->fini();
        return 1;
    }

    if (!read_file(log_path, log_text, sizeof(log_text)) ||
        expect_contains(log_text, "open manager fifo failed") != 0 ||
        expect_contains(log_text, socket_path) != 0) {
        sdk->fini();
        return 1;
    }

    sdk->fini();
    unlink(log_path);
    return 0;
}

static int expect_response_timeout_detail(const ubse_ssu_sdk_ops_t *sdk,
                                          sdk_last_error_fn last_error,
                                          const char *root)
{
    char socket_path[256];
    char log_path[256];
    char log_text[4096];
    ssu_resource_info_t resources[1];
    uint32_t count = 1;
    pid_t child;
    ssu_err_t err;
    int status = 0;
    int ready_pipe[2];
    char ready_byte;

    snprintf(socket_path, sizeof(socket_path), "%s/no-response.fifo", root);
    snprintf(log_path, sizeof(log_path), "%s/sdk-timeout.log", root);
    unlink(socket_path);
    unlink(log_path);

    if (mkfifo(socket_path, 0600) != 0) {
        perror("mkfifo");
        return 1;
    }

    if (pipe(ready_pipe) != 0) {
        perror("pipe");
        unlink(socket_path);
        return 1;
    }

    child = fork();
    if (child < 0) {
        perror("fork");
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        unlink(socket_path);
        return 1;
    }

    if (child == 0) {
        char buf[512];
        close(ready_pipe[0]);
        int fd = open(socket_path, O_RDWR);
        if (fd < 0) {
            _exit(2);
        }
        (void)write(ready_pipe[1], "1", 1);
        close(ready_pipe[1]);
        (void)read(fd, buf, sizeof(buf));
        close(fd);
        _exit(0);
    }

    close(ready_pipe[1]);
    if (read(ready_pipe[0], &ready_byte, 1) != 1) {
        perror("read ready pipe");
        close(ready_pipe[0]);
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        unlink(socket_path);
        return 1;
    }
    close(ready_pipe[0]);

    setenv("SSU_MGR_SOCKET", socket_path, 1);
    setenv("UBSE_SSU_SDK_LOG", log_path, 1);
    setenv("UBSE_SSU_SDK_RESPONSE_TIMEOUT_MS", "200", 1);

    err = sdk->init(NULL);
    if (err != SSU_OK) {
        fprintf(stderr, "sdk init failed: %d\n", err);
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        unlink(socket_path);
        return 1;
    }

    memset(resources, 0, sizeof(resources));
    alarm(3);
    err = sdk->list(resources, &count);
    alarm(0);

    if (err != SSU_ERR_IO) {
        fprintf(stderr, "expected list to return SSU_ERR_IO, got %d\n",
                err);
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        sdk->fini();
        unlink(socket_path);
        return 1;
    }

    if (waitpid(child, &status, 0) < 0) {
        perror("waitpid");
        sdk->fini();
        unlink(socket_path);
        return 1;
    }

    if (expect_contains(last_error(), "timeout waiting for manager response") != 0 ||
        expect_contains(last_error(), "response_fifo=/tmp/ubse-ssu-sdk-response-") != 0 ||
        expect_contains(last_error(), "hint=") != 0) {
        sdk->fini();
        unlink(socket_path);
        return 1;
    }

    if (!read_file(log_path, log_text, sizeof(log_text)) ||
        expect_contains(log_text, "timeout waiting for manager response") != 0 ||
        expect_contains(log_text, socket_path) != 0) {
        sdk->fini();
        unlink(socket_path);
        return 1;
    }

    sdk->fini();
    unlink(socket_path);
    unlink(log_path);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    char root_template[] = "/tmp/ubse-sdk-diagnostics-XXXXXX";
    char *root;
    void *handle;
    sdk_entry_fn entry;
    sdk_last_error_fn last_error;
    const ubse_ssu_sdk_ops_t *sdk;
    int failed = 0;

    if (argc != 2) {
        fputs("usage: sdk_diagnostics LIBUBSE_SSU_SDK_SO\n", stderr);
        return 1;
    }

    root = mkdtemp(root_template);
    if (root == NULL) {
        perror("mkdtemp");
        return 1;
    }

    handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    entry = load_symbol<sdk_entry_fn>(handle, "ubse_ssu_sdk_entry");
    last_error = load_symbol<sdk_last_error_fn>(
        handle, "ubse_ssu_sdk_last_error");
    if (entry == NULL || last_error == NULL) {
        dlclose(handle);
        return 1;
    }

    sdk = entry();
    if (sdk == NULL || sdk->struct_size < sizeof(*sdk) ||
        sdk->init == NULL || sdk->fini == NULL ||
        sdk->list == NULL) {
        fputs("ubse_ssu_sdk_entry returned incomplete ops table\n", stderr);
        dlclose(handle);
        return 1;
    }

    failed |= expect_missing_manager_detail(sdk, last_error, root);
    failed |= expect_response_timeout_detail(sdk, last_error, root);

    dlclose(handle);
    rmdir(root);
    return failed == 0 ? 0 : 1;
}
