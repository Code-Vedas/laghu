// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "server_internal.h"

#ifdef _WIN32
extern volatile LONG proxy_stop_requests;
static SERVICE_STATUS_HANDLE proxy_service_handle;
static SERVICE_STATUS proxy_service_status;
static const laghu_proxy_options *proxy_service_options;

static DWORD WINAPI proxy_service_control(DWORD control, DWORD event_type,
                                          LPVOID event_data, LPVOID context) {
  (void)event_type;
  (void)event_data;
  (void)context;
  if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
    InterlockedIncrement(&proxy_stop_requests);
    proxy_service_status.dwCurrentState = SERVICE_STOP_PENDING;
    proxy_service_status.dwWaitHint =
        proxy_service_options->drain_timeout * 1000U;
    (void)SetServiceStatus(proxy_service_handle, &proxy_service_status);
    return NO_ERROR;
  }
  return ERROR_CALL_NOT_IMPLEMENTED;
}

static void WINAPI proxy_service_main(DWORD argument_count, LPSTR *arguments) {
  int result;
  (void)argument_count;
  (void)arguments;
  memset(&proxy_service_status, 0, sizeof(proxy_service_status));
  proxy_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  proxy_service_status.dwCurrentState = SERVICE_START_PENDING;
  proxy_service_status.dwControlsAccepted =
      SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
  proxy_service_handle =
      RegisterServiceCtrlHandlerExA("laghu", proxy_service_control, NULL);
  if (proxy_service_handle == NULL) return;
  (void)SetServiceStatus(proxy_service_handle, &proxy_service_status);
  proxy_service_status.dwCurrentState = SERVICE_RUNNING;
  (void)SetServiceStatus(proxy_service_handle, &proxy_service_status);
  result = laghu_proxy_run(proxy_service_options);
  proxy_service_status.dwCurrentState = SERVICE_STOPPED;
  proxy_service_status.dwWin32ExitCode =
      result == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR;
  proxy_service_status.dwServiceSpecificExitCode = (DWORD)result;
  (void)SetServiceStatus(proxy_service_handle, &proxy_service_status);
}

int laghu_proxy_run_service(const laghu_proxy_options *options) {
  SERVICE_TABLE_ENTRYA table[] = {{"laghu", proxy_service_main}, {NULL, NULL}};
  proxy_service_options = options;
  return StartServiceCtrlDispatcherA(table) != 0 ? 0 : 1;
}
#endif
