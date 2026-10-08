/* H2 host diagnostic only, not a controller/driver installer.
 * Writes only to the exact unique serial + VID/PID/version + vendor TLC/caps
 * created by brickpro-functionfs-control-probe.c. An independent parent bounds
 * even synchronous HidD requests; overlapped buffers stay alive until complete
 * or process exit. Never sends packets to an existing game controller.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int valid_serial(const char *s) {
	if (strncmp(s,"nxep0.",6) || strlen(s)>79) return 0;
	const char *p=s+6;
	if (*p<'0' || *p>'9') return 0;
	while (*p>='0' && *p<='9') ++p;
	if (*p++!='.' || *p<'0' || *p>'9') return 0;
	while (*p>='0' && *p<='9') ++p;
	return !*p;
}
static void pattern(unsigned char *report,unsigned base) {
	report[0]=0; /* Windows dummy report ID, not part of the 32-byte USB payload. */
	for (unsigned i=0;i<32;++i) report[i+1]=(unsigned char)(base+(i&15));
}
static HANDLE find_target(const wchar_t *serial) {
	GUID guid; HidD_GetHidGuid(&guid);
	HDEVINFO set=SetupDiGetClassDevsW(&guid,NULL,NULL,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
	if (set==INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
	HANDLE found=INVALID_HANDLE_VALUE;
	for (DWORD i=0;;++i) {
		SP_DEVICE_INTERFACE_DATA data={0}; data.cbSize=sizeof(data);
		if (!SetupDiEnumDeviceInterfaces(set,NULL,&guid,i,&data)) break;
		DWORD needed=0;
		(void)SetupDiGetDeviceInterfaceDetailW(set,&data,NULL,0,&needed,NULL);
		if (needed<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || needed>65536) continue;
		SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail=calloc(1,needed);
		if (!detail) break;
		detail->cbSize=sizeof(*detail);
		if (!SetupDiGetDeviceInterfaceDetailW(set,&data,detail,needed,NULL,NULL) ||
			!wcsstr(detail->DevicePath,L"vid_18d1&pid_d002")) { free(detail); continue; }
		HANDLE h=CreateFileW(detail->DevicePath,GENERIC_READ|GENERIC_WRITE,
			FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,NULL);
		free(detail); if (h==INVALID_HANDLE_VALUE) continue;
		HIDD_ATTRIBUTES attributes={0}; attributes.Size=sizeof(attributes);
		wchar_t actual[126]={0},product[126]={0};
		PHIDP_PREPARSED_DATA parsed=NULL; HIDP_CAPS caps={0};
		int match=HidD_GetAttributes(h,&attributes) && attributes.VendorID==0x18d1 &&
			attributes.ProductID==0xd002 && attributes.VersionNumber==0x9903 &&
			HidD_GetSerialNumberString(h,actual,sizeof(actual)) && !wcscmp(actual,serial) &&
			HidD_GetProductString(h,product,sizeof(product)) && !wcscmp(product,L"NX Controller ep0 lab") &&
			HidD_GetPreparsedData(h,&parsed) && HidP_GetCaps(parsed,&caps)==HIDP_STATUS_SUCCESS &&
			caps.UsagePage==0xff00 && caps.Usage==1 && caps.InputReportByteLength==9 &&
			caps.OutputReportByteLength==33 && caps.FeatureReportByteLength==0;
		if (parsed) (void)HidD_FreePreparsedData(parsed);
		if (match) { found=h; printf("target_verified serial=%ls vid=18d1 pid=d002 version=9903 usage=ff00:0001 input=9 output=33 feature=0\n",serial); break; }
		CloseHandle(h);
	}
	SetupDiDestroyDeviceInfoList(set); return found;
}
static int overlapped_io(HANDLE h,int reading,unsigned char *buffer,DWORD size,DWORD *transferred) {
	OVERLAPPED ol={0}; ol.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL); if (!ol.hEvent) return 0;
	BOOL ok=reading ? ReadFile(h,buffer,size,transferred,&ol) : WriteFile(h,buffer,size,transferred,&ol);
	DWORD error=ok ? 0:GetLastError();
	if (!ok && error==ERROR_IO_PENDING) {
		DWORD wait=WaitForSingleObject(ol.hEvent,2000);
		if (wait==WAIT_OBJECT_0) { ok=GetOverlappedResult(h,&ol,transferred,FALSE); error=ok?0:GetLastError(); }
		else {
			(void)CancelIoEx(h,&ol);
			if (WaitForSingleObject(ol.hEvent,500)!=WAIT_OBJECT_0) {
				printf("io_cancel_not_completed read=%d; exiting worker without releasing pending buffer\n",reading);
				fflush(stdout); ExitProcess(3); /* Process teardown owns pending IO memory/handles. */
			}
			(void)GetOverlappedResult(h,&ol,transferred,FALSE); ok=FALSE; error=WAIT_TIMEOUT;
		}
	}
	CloseHandle(ol.hEvent);
	printf("%s ok=%d bytes=%lu error=%lu\n",reading?"ReadFile":"WriteFile",(int)ok,(unsigned long)*transferred,(unsigned long)error);
	return ok!=FALSE;
}
static int worker(const char *serial) {
	wchar_t wanted[80]; for (size_t i=0;i<=strlen(serial);++i) wanted[i]=(wchar_t)(unsigned char)serial[i];
	ULONGLONG deadline=GetTickCount64()+8000;
	HANDLE h=INVALID_HANDLE_VALUE;
	do { h=find_target(wanted); if (h!=INVALID_HANDLE_VALUE) break; Sleep(100); } while (GetTickCount64()<deadline);
	if (h==INVALID_HANDLE_VALUE) { puts("target_not_found timeout=8000ms no_output_sent"); return 2; }
	unsigned char input[9]={0}; DWORD bytes=0;
	int input_ok=overlapped_io(h,1,input,sizeof(input),&bytes) && bytes==sizeof(input);
	for (unsigned i=0;i<sizeof(input);++i) if (input[i]) input_ok=0;
	printf("neutral_input_verified=%d\n",input_ok);
	unsigned char report[33]; pattern(report,0xa0);
	SetLastError(0); BOOLEAN a=HidD_SetOutputReport(h,report,sizeof(report)); DWORD error=a?0:GetLastError();
	printf("HidD_SetOutputReport phase=A ok=%d host_size=33 error=%lu\n",(int)a,(unsigned long)error);
	pattern(report,0xc0); SetLastError(0);
	BOOLEAN bad=HidD_SetOutputReport(h,report,sizeof(report)); error=bad?0:GetLastError();
	printf("HidD_SetOutputReport invalid_payload ok=%d host_size=33 error=%lu (transport result only; device journal decides rejection)\n",(int)bad,(unsigned long)error);
	pattern(report,0xa0); SetLastError(0);
	BOOLEAN short_report=HidD_SetOutputReport(h,report,sizeof(report)-1); error=short_report?0:GetLastError();
	printf("HidD_SetOutputReport short_buffer ok=%d host_size=32 error=%lu (may be rejected by Windows before USB)\n",(int)short_report,(unsigned long)error);
	pattern(report,0xb0); bytes=0;
	int b=overlapped_io(h,0,report,sizeof(report),&bytes) && bytes==sizeof(report);
	CloseHandle(h);
	printf("host_result input=%d control_A=%d write_B=%d; correlate with device output_mask=3\n",input_ok,(int)a,b);
	return input_ok && a && b ? 0:1;
}
int main(int argc,char **argv) {
	setvbuf(stdout,NULL,_IONBF,0);
	if (argc==2 && !strcmp(argv[1],"--selftest")) {
		unsigned char report[33]; pattern(report,0xa0);
		int ok=valid_serial("nxep0.123.456") && !valid_serial("nxep0.123.456x") &&
			!valid_serial("nxep0..456") && !valid_serial("nxep0.123.") && !valid_serial("existing-controller") &&
			report[0]==0 && report[1]==0xa0 && report[32]==0xaf;
		printf("HOST_PURE_SELFTEST passed=%d (no device opened)\n",ok); return ok?0:1;
	}
	if (argc!=3 || !valid_serial(argv[2])) return 2;
	if (!strcmp(argv[1],"--worker")) return worker(argv[2]);
	if (strcmp(argv[1],"--run")) return 2;
	wchar_t exe[32768],command[33000],serial[80];
	DWORD length=GetModuleFileNameW(NULL,exe,32768); if (!length || length>=32768) return 2;
	for (size_t i=0;i<=strlen(argv[2]);++i) serial[i]=(wchar_t)(unsigned char)argv[2][i];
	if (swprintf(command,33000,L"\"%ls\" --worker %ls",exe,serial)<0) return 2;
	STARTUPINFOW si={0}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES;
	si.hStdInput=GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE); si.hStdError=GetStdHandle(STD_ERROR_HANDLE);
	PROCESS_INFORMATION pi={0};
	if (!CreateProcessW(exe,command,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi)) return 2;
	CloseHandle(pi.hThread);
	DWORD wait=WaitForSingleObject(pi.hProcess,16000),code=2;
	if (wait==WAIT_OBJECT_0) (void)GetExitCodeProcess(pi.hProcess,&code);
	else {
		(void)TerminateProcess(pi.hProcess,3); (void)WaitForSingleObject(pi.hProcess,1000);
		puts("host_worker_timeout terminated_owned_worker no_other_process_touched"); code=3;
	}
	CloseHandle(pi.hProcess); printf("host_probe_rc=%lu\n",(unsigned long)code); return (int)code;
}
