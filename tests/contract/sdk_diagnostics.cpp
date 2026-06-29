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

static int read_line(int fd, char *buf, size_t n)
{
    size_t used = 0;

    if (buf == NULL || n == 0) {
        return 0;
    }

    while (used + 1 < n) {
        char ch;
        ssize_t got = read(fd, &ch, 1);

        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }

        if (got == 0) {
            break;
        }

        buf[used++] = ch;
        if (ch == '\n') {
            break;
        }
    }

    buf[used] = '\0';
    return used > 0;
}

static const char *fake_response_for_command(const char *command)
{
    if (strncmp(command, "LIST", 4) == 0) {
        return "OK\npool entries: 1\nmock-ssu0 host0 ONLINE 0/1048576\n";
    }
    if (strncmp(command, "ALLOCATE ", 9) == 0) {
        return "OK\nreq-audit-0\n";
    }
    if (strncmp(command, "ALLOCATE_RESULT ", 16) == 0) {
        return "OK\n/dev/ssu/audit-disk\nphysical 0 mock-ssu0 1 0 1048576 lba=0\n";
    }
    if (strncmp(command, "MOUNT_DEV ", 10) == 0 ||
        strncmp(command, "UNMOUNT ", 8) == 0 ||
        strncmp(command, "FREE ", 5) == 0) {
        return "OK\n";
    }
    return "ERR -1\nunknown command\n";
}

static void fake_manager_loop(const char *socket_path, int ready_fd,
                              unsigned int expected_requests)
{
    int fd = open(socket_path, O_RDWR);
    unsigned int handled = 0;

    if (fd < 0) {
        _exit(2);
    }

    (void)write(ready_fd, "1", 1);
    close(ready_fd);

    while (handled < expected_requests) {
        char line[1024];
        char response_path[256];
        char *command;
        char *space;
        int response_fd;
        const char *response;

        if (!read_line(fd, line, sizeof(line))) {
            break;
        }

        space = strchr(line, ' ');
        if (space == NULL) {
            break;
        }
        *space = '\0';
        command = space + 1;
        command[strcspn(command, "\r\n")] = '\0';
        strncpy(response_path, line, sizeof(response_path) - 1);
        response_path[sizeof(response_path) - 1] = '\0';

        response = fake_response_for_command(command);
        response_fd = open(response_path, O_WRONLY);
        if (response_fd < 0) {
            break;
        }
        (void)write(response_fd, response, strlen(response));
        close(response_fd);
        handled++;
    }

    close(fd);
    _exit(handled == expected_requests ? 0 : 3);
}

static int expect_success_audit_log(const ubse_ssu_sdk_ops_t *sdk,
                                    const char *root)
{
    char socket_path[256];
    char log_path[256];
    char log_text[8192];
    ssu_resource_info_t resources[2];
    ssu_api_allocate_req_t req;
    ssu_api_allocate_resp_t resp;
    ssu_api_allocate_result_info_t result;
    uint32_t count = 2;
    const char *hosts[] = {"host0"};
    pid_t child;
    int status = 0;
    int ready_pipe[2];
    char ready_byte;
    int failed = 0;

    snprintf(socket_path, sizeof(socket_path), "%s/audit-manager.fifo",
             root);
    snprintf(log_path, sizeof(log_path), "%s/sdk-audit.log", root);
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
        close(ready_pipe[0]);
        fake_manager_loop(socket_path, ready_pipe[1], 6);
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
    unsetenv("UBSE_SSU_SDK_RESPONSE_TIMEOUT_MS");

    if (sdk->init(NULL) != SSU_OK) {
        fputs("sdk init failed in audit flow\n", stderr);
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        unlink(socket_path);
        return 1;
    }

    memset(resources, 0, sizeof(resources));
    if (sdk->list(resources, &count) != SSU_OK) {
        fputs("sdk list failed in audit flow\n", stderr);
        failed = 1;
    }

    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    req.size_bytes = 1048576;
    req.user_id = "user-audit";
    req.physical_disk_count = 1;
    req.logical_disk_aggregate = 1;
    req.allocation_type = SSU_SHARE_EXCLUSIVE;
    req.host_ids = hosts;
    req.host_count = 1;
    req.disk_name = "audit-disk";
    if (!failed && sdk->allocate(&req, &resp) != SSU_OK) {
        fputs("sdk allocate failed in audit flow\n", stderr);
        failed = 1;
    }

    memset(&result, 0, sizeof(result));
    if (!failed &&
        sdk->allocate_result_get(resp.request_id, &result) != SSU_OK) {
        fputs("sdk allocate_result_get failed in audit flow\n", stderr);
        failed = 1;
    }

    if (!failed && sdk->mount(result.device_path, "host0") != SSU_OK) {
        fputs("sdk mount failed in audit flow\n", stderr);
        failed = 1;
    }

    if (!failed && sdk->unmount(result.device_path) != SSU_OK) {
        fputs("sdk unmount failed in audit flow\n", stderr);
        failed = 1;
    }

    if (!failed && sdk->free(result.device_path) != SSU_OK) {
        fputs("sdk free failed in audit flow\n", stderr);
        failed = 1;
    }

    sdk->fini();

    if (waitpid(child, &status, 0) < 0) {
        perror("waitpid");
        unlink(socket_path);
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "fake manager exited unexpectedly status=%d\n",
                status);
        failed = 1;
    }

    if (!read_file(log_path, log_text, sizeof(log_text))) {
        failed = 1;
    }

    failed |= expect_contains(log_text, "audit api=init event=begin");
    failed |= expect_contains(log_text, "audit api=init event=end result=SSU_OK(0)");
    failed |= expect_contains(log_text, "audit api=list event=begin");
    failed |= expect_contains(log_text, "audit api=list event=end result=SSU_OK(0)");
    failed |= expect_contains(log_text, "audit api=allocate event=begin");
    failed |= expect_contains(log_text, "audit api=allocate event=end result=SSU_OK(0)");
    failed |= expect_contains(log_text, "request_id=req-audit-0");
    failed |= expect_contains(log_text, "audit api=allocate_result_get event=begin");
    failed |= expect_contains(log_text, "audit api=allocate_result_get event=end result=SSU_OK(0)");
    failed |= expect_contains(log_text, "device_path=/dev/ssu/audit-disk");
    failed |= expect_contains(log_text, "audit api=mount event=begin");
    failed |= expect_contains(log_text, "audit api=mount event=end result=SSU_OK(0)");
    failed |= expect_contains(log_text, "audit api=unmount event=begin");
    failed |= expect_contains(log_text, "audit api=unmount event=end result=SSU_OK(0)");
    failed |= expect_contains(log_text, "audit api=free event=begin");
    failed |= expect_contains(log_text, "audit api=free event=end result=SSU_OK(0)");
    failed |= expect_contains(log_text, "audit api=fini event=begin");
    failed |= expect_contains(log_text, "audit api=fini event=end result=SSU_OK(0)");
    failed |= expect_contains(log_text, "audit manager event=send op=LIST");
    failed |= expect_contains(log_text, "audit manager event=status op=LIST result=SSU_OK(0)");

    unlink(socket_path);
    unlink(log_path);
    return failed;
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
    failed |= expect_success_audit_log(sdk, root);

    dlclose(handle);
    rmdir(root);
    return failed == 0 ? 0 : 1;
}
