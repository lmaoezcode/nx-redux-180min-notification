/* Diagnostic H2 only. Vendor-defined HID: one IN endpoint; OUT on ep0.
 * No input, motor, radio, display, storage export or firmware writes.
 * Guardian is armed before any pause/unbind/function creation. Unique names,
 * canonical link checks, exact process identities and serialized restore.
 * Live is disabled unless deliberately compiled for the audited boot session.
 */
#define main nx_owner_main_unused
#include "brickpro-usb-owner-probe.c"
#undef main
#include <assert.h>
#include <endian.h>
#include <linux/hid.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <mntent.h>
#include <sys/mount.h>

#define ROOT "/sys/kernel/config/usb_gadget/g1"
#define CONFIG ROOT "/configs/c.1"
#define MTP_EXE "/mnt/SDCARD/.system/bin/umtprd"
#define AUDITED_BOOT "6932e5dc-b767-44c9-8f0c-024c20c94b4e"

/* Vendor TLC deliberately avoids adding a phantom game controller. */
static const uint8_t report_desc[] = {
	0x06,0x00,0xff, 0x09,0x01, 0xa1,0x01,
	0x15,0x00, 0x26,0xff,0x00, 0x75,0x08,
	0x95,0x08, 0x09,0x01, 0x81,0x02,
	0x95,0x20, 0x09,0x02, 0x91,0x02, 0xc0
};
struct __attribute__((packed)) HidDesc {
	uint8_t length,type; uint16_t version; uint8_t country,count,report_type;
	uint16_t report_length;
};
struct __attribute__((packed)) Descriptors {
	struct usb_functionfs_descs_head_v2 header;
	uint32_t fs_count,hs_count;
	struct usb_interface_descriptor fs_interface;
	struct HidDesc fs_hid;
	struct usb_endpoint_descriptor_no_audio fs_ep;
	struct usb_interface_descriptor hs_interface;
	struct HidDesc hs_hid;
	struct usb_endpoint_descriptor_no_audio hs_ep;
};
#define INTERFACE {9,USB_DT_INTERFACE,0,0,1,USB_INTERFACE_CLASS_HID,0,0,1}
#define HID_DESC {9,HID_DT_HID,0x0111,0,1,HID_DT_REPORT,sizeof(report_desc)}
#define EP(interval) {7,USB_DT_ENDPOINT,0x81,USB_ENDPOINT_XFER_INT,8,interval}
static const struct Descriptors descriptors = {
	{FUNCTIONFS_DESCRIPTORS_MAGIC_V2,sizeof(struct Descriptors),FUNCTIONFS_HAS_FS_DESC|FUNCTIONFS_HAS_HS_DESC},
	3,3,INTERFACE,HID_DESC,EP(4),INTERFACE,HID_DESC,EP(6)
};
struct __attribute__((packed)) Strings {
	struct usb_functionfs_strings_head header; uint16_t language;
	char name[sizeof("NX Controller ep0 lab")];
};
static const struct Strings strings = {
	{FUNCTIONFS_STRINGS_MAGIC,sizeof(struct Strings),1,1},0x0409,"NX Controller ep0 lab"
};
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "Probe is for little-endian targets");
_Static_assert(sizeof(struct Descriptors)==70, "FunctionFS golden size");
_Static_assert(sizeof(struct HidDesc)==9, "HID golden size");

struct Attribute { char path[512],original[256],lab[256]; };
struct Context {
	struct Identity mtp;
	char name[80],function[512],link[512],mountpoint[512],mtp_link[512],mtp_function[512],mtp_target[512];
	struct Attribute attributes[3];
	int ep0,epin; bool fixture,enabled;
	unsigned mask,rejected,setups,neutral_reports,fault_at;
	int64_t completed_at;
};
static volatile sig_atomic_t io_timed_out;
static void request_timeout(int sig) { (void)sig; io_timed_out=1; }
static unsigned payload_phase(const uint8_t *data, size_t size) {
	if (size!=32) return 0;
	unsigned base = data[0]==0xa0 ? 0xa0 : data[0]==0xb0 ? 0xb0 : 0;
	if (!base) return 0;
	for (size_t i=0;i<size;++i) if (data[i]!=(uint8_t)(base+(i&15))) return 0;
	return base==0xa0 ? 1 : 2;
}
static bool output_setup(const struct usb_ctrlrequest *r) {
	return r->bRequestType==0x21 && r->bRequest==HID_REQ_SET_REPORT &&
		le16toh(r->wValue)==0x0200 && le16toh(r->wIndex)==0 && le16toh(r->wLength)==32;
}
static bool pause_identity(const struct Identity *id) {
	if (!same_process(id) || kill(id->pid,SIGSTOP)) return false;
	int64_t deadline=now_ms()+1000;
	while (!all_stopped(id) && now_ms()<deadline) pause_ms(10);
	return all_stopped(id);
}
static bool resume_mtp(const struct Snapshot *s) {
	struct Context *c=s->context;
	if (!same_process(&c->mtp) || kill(c->mtp.pid,SIGCONT)) {
		record(s,"mtp_resume_identity_or_signal_failed",errno); return false;
	}
	struct Identity after={0};
	int64_t deadline=now_ms()+500;
	do {
		if (!identity(c->mtp.pid,&after) && after.start==c->mtp.start && after.state!='T' && after.state!='t') {
			record(s,"mtp_original_instance_resumed",0); return true;
		}
		pause_ms(10);
	} while (now_ms()<deadline);
	return false;
}
static bool no_sd_regular_fds(const struct Identity *id) {
	char directory[80]; snprintf(directory,sizeof(directory),"/proc/%ld/fd",(long)id->pid);
	DIR *dir=opendir(directory); if (!dir) return false;
	struct dirent *entry; bool safe=true;
	while ((entry=readdir(dir))) {
		if (entry->d_name[0]=='.') continue;
		char path[512],target[512]; struct stat st;
		snprintf(path,sizeof(path),"%s/%s",directory,entry->d_name);
		ssize_t n=readlink(path,target,sizeof(target)-1);
		if (n<0 || (size_t)n==sizeof(target)-1) { safe=false; break; }
		target[n]=0;
		if (!strncmp(target,"/mnt/SDCARD/",12)) {
			if (stat(path,&st) || S_ISREG(st.st_mode)) {
				safe=false; break; /* No filenames/content from a pending transfer in logs. */
			}
		}
	}
	closedir(dir); return safe;
}
static bool before_unbind(const struct Snapshot *s) {
	struct Context *c=s->context;
	if (!pause_identity(&c->mtp)) return false;
	record(s,"mtp_original_instance_stopped",0);
	return c->fixture || (no_sd_regular_fds(&c->mtp) && no_sd_regular_fds(&s->adb));
}
static void release_endpoints(const struct Snapshot *s) {
	struct Context *c=s->context;
	if (c->epin>=0) { close(c->epin); c->epin=-1; }
	if (c->ep0>=0) { close(c->ep0); c->ep0=-1; }
}
static bool mounted_here(const char *path) {
	FILE *file=setmntent("/proc/self/mounts","r"); if (!file) return true; /* fail closed on umount */
	struct mntent *entry; bool found=false;
	while ((entry=getmntent(file))) if (!strcmp(entry->mnt_dir,path)) { found=true; break; }
	endmntent(file); return found;
}
static int remove_owned_link(const char *path,const char *target) {
	struct stat st;
	if (lstat(path,&st)) return errno==ENOENT ? 0 : -1;
	if (!link_matches(path,target)) { errno=EPERM; return -1; }
	return unlink(path);
}
static bool allowed_links(const struct Context *c) {
	char directory[512]; snprintf(directory,sizeof(directory),"%s",c->link);
	char *slash=strrchr(directory,'/'); if (!slash) return false; *slash=0;
	DIR *dir=opendir(directory); if (!dir) return false;
	struct dirent *entry; bool safe=true;
	while ((entry=readdir(dir))) {
		char path[1024]; struct stat st;
		snprintf(path,sizeof(path),"%s/%s",directory,entry->d_name);
		if (lstat(path,&st)) { safe=false; break; }
		if (!S_ISLNK(st.st_mode)) continue;
		if (strcmp(entry->d_name,"ffs.adb") && strcmp(path,c->mtp_link) && strcmp(path,c->link)) { safe=false; break; }
		char expected[1024];
		if (!strcmp(entry->d_name,"ffs.adb")) {
			snprintf(expected,sizeof(expected),"%s/../../functions/ffs.adb",directory);
			if (!link_matches(path,expected)) { safe=false; break; }
		} else if (!link_matches(path,!strcmp(path,c->mtp_link)?c->mtp_function:c->function)) {
			safe=false; break;
		}
	}
	closedir(dir); return safe;
}
static int cleanup(const struct Snapshot *s) {
	struct Context *c=s->context; char value[128];
	if (!pause_identity(&c->mtp) || !allowed_links(c) ||
		read_value(s->udc_path,value,sizeof(value)) || !udc_unbound(value)) return 1;
	if (remove_owned_link(c->link,c->function)) { record(s,"cleanup_foreign_link_or_unlink_failed",errno); return 1; }
	release_endpoints(s);
	if (!c->fixture && mounted_here(c->mountpoint) && umount(c->mountpoint)) {
		record(s,"cleanup_lab_umount_failed",errno); return 1;
	}
	if (c->fixture) {
		char path[600]; snprintf(path,sizeof(path),"%s/ep0",c->mountpoint); (void)unlink(path);
	}
	if (rmdir(c->mountpoint) && errno!=ENOENT) { record(s,"cleanup_lab_mount_dir_failed",errno); return 1; }
	if (rmdir(c->function) && errno!=ENOENT) { record(s,"cleanup_lab_function_failed",errno); return 1; }
	struct stat st;
	if (!lstat(c->mtp_link,&st)) {
		if (!link_matches(c->mtp_link,c->mtp_function)) return 1;
	/* ConfigFS resolves a userspace symlink(2) target from the caller's cwd,
	 * while readlink exposes a generated relative path. Recreate with the
	 * canonical absolute function path, then verify the resolved target. */
	} else if (errno!=ENOENT || symlink(c->mtp_function,c->mtp_link)) return 1;
	if (!link_matches(c->mtp_link,c->mtp_function)) return 1;
	for (unsigned i=0;i<3;++i) {
		char current[256]; struct Attribute *a=&c->attributes[i];
		if (read_value(a->path,current,sizeof(current)) ||
			(strcmp(current,a->original) && strcmp(current,a->lab))) {
			record(s,"restore_foreign_identity_refused",i); return 1;
		}
		if (strcmp(current,a->original) && write_value(a->path,a->original)) return 1;
		if (read_value(a->path,current,sizeof(current)) || strcmp(current,a->original)) return 1;
	}
	record(s,"lab_removed_original_link_identity_verified",0); return 0;
}
static void fault(const struct Snapshot *s,unsigned stage) {
	struct Context *c=s->context;
	if (c->fixture && c->fault_at==stage) (void)kill(getpid(),SIGKILL);
}
static bool activate(const struct Snapshot *s) {
	struct Context *c=s->context;
	if (!all_stopped(&c->mtp) || !allowed_links(c) || !link_matches(c->mtp_link,c->mtp_function)) return false;
	if (mkdir(c->function,0700)) { record(s,"lab_function_create_failed",errno); return false; }
	fault(s,1);
	if (mkdir(c->mountpoint,0700)) { record(s,"lab_mount_dir_create_failed",errno); return false; }
	if (!c->fixture && mount(c->name,c->mountpoint,"functionfs",0,NULL)) { record(s,"lab_mount_failed",errno); return false; }
	fault(s,2);
	char path[600]; snprintf(path,sizeof(path),"%s/ep0",c->mountpoint);
	c->ep0=open(path,O_RDWR|O_NONBLOCK|O_CLOEXEC|(c->fixture ? O_CREAT|O_EXCL : 0),0600);
	if (c->ep0<0) { record(s,"lab_ep0_open_failed",errno); return false; }
	if (write(c->ep0,&descriptors,sizeof(descriptors))!=sizeof(descriptors)) { record(s,"lab_descriptors_write_failed",errno); return false; }
	if (write(c->ep0,&strings,sizeof(strings))!=sizeof(strings)) { record(s,"lab_strings_write_failed",errno); return false; }
	record(s,"one_in_descriptor_parser_ready",sizeof(descriptors));
	fault(s,3);
	if (remove_owned_link(c->mtp_link,c->mtp_function)) { record(s,"mtp_link_takeover_failed",errno); return false; }
	fault(s,4);
	if (symlink(c->function,c->link)) { record(s,"lab_link_create_failed",errno); return false; }
	fault(s,5);
	for (unsigned i=0;i<3;++i) if (write_value(c->attributes[i].path,c->attributes[i].lab)) { record(s,"lab_identity_write_failed",errno); return false; }
	fault(s,6);
	if (write_value(s->udc_path,s->udc)) { record(s,"lab_bind_failed",errno); return false; }
	record(s,"one_in_lab_bound",0);
	fault(s,7);
	if (!c->fixture) {
		snprintf(path,sizeof(path),"%s/ep1",c->mountpoint);
		c->epin=open(path,O_WRONLY|O_NONBLOCK|O_CLOEXEC);
		if (c->epin<0) { record(s,"lab_ep1_open_failed",errno); return false; }
	}
	return true;
}
static bool control_io(struct Context *c,bool in,void *buffer,size_t size) {
	io_timed_out=0;
	(void)alarm(1);
	ssize_t n=in ? write(c->ep0,buffer,size) : read(c->ep0,buffer,size);
	int saved=errno; (void)alarm(0);
	if (n==(ssize_t)size) return true;
	if (n<0 && (saved==EIDRM || saved==EL2HLT || saved==ECONNRESET || saved==ESHUTDOWN)) return true;
	return false;
}
static int handle_setup(const struct Snapshot *s,const struct usb_ctrlrequest *r) {
	struct Context *c=s->context;
	uint16_t length=le16toh(r->wLength),value=le16toh(r->wValue),index=le16toh(r->wIndex);
	if (++c->setups>200) return 1; /* Log remains bounded; unknown host flood stops the lab. */
	dprintf(s->log_fd,"setup type=%02x request=%02x value=%04x index=%u length=%u\n",r->bRequestType,r->bRequest,value,index,length);
	uint8_t data[32]={0};
	if (output_setup(r)) {
		io_timed_out=0;
		(void)alarm(1); ssize_t n=read(c->ep0,data,sizeof(data)); int saved=errno; (void)alarm(0);
		if (n<0 && (saved==EIDRM || saved==ESHUTDOWN || saved==ECONNRESET)) return 0;
		if (n!=32) return 1;
		unsigned phase=payload_phase(data,sizeof(data));
		if (phase) { c->mask|=phase; record(s,"valid_control_output_phase",phase); }
		else { ++c->rejected; record(s,"invalid_control_payload_ignored",32); }
		dprintf(s->log_fd,"control_payload=");
		for (size_t i=0;i<sizeof(data);++i) dprintf(s->log_fd,"%02x",data[i]);
		dprintf(s->log_fd,"\n");
		if (c->mask==3 && !c->completed_at) c->completed_at=now_ms();
		return 0;
	}
	if (index==0 && r->bRequestType==0x81 && r->bRequest==USB_REQ_GET_DESCRIPTOR) {
		if (value==0x2200) return control_io(c,true,(void*)report_desc,length<sizeof(report_desc)?length:sizeof(report_desc)) ? 0:1;
		if (value==0x2100) { struct HidDesc h=HID_DESC; return control_io(c,true,&h,length<sizeof(h)?length:sizeof(h)) ? 0:1; }
	}
	if (index==0 && r->bRequestType==0xa1 && r->bRequest==HID_REQ_GET_REPORT && value==0x0100)
		return control_io(c,true,data,length<8?length:8) ? 0:1;
	if (index==0 && r->bRequestType==0xa1 && (r->bRequest==HID_REQ_GET_IDLE || r->bRequest==HID_REQ_GET_PROTOCOL)) {
		data[0]=r->bRequest==HID_REQ_GET_PROTOCOL ? 1:0;
		return control_io(c,true,data,length<1?length:1) ? 0:1;
	}
	if (index==0 && r->bRequestType==0x21 && !length &&
		(r->bRequest==HID_REQ_SET_IDLE || (r->bRequest==HID_REQ_SET_PROTOCOL && value==1)))
		return control_io(c,false,data,0) ? 0:1;
	/* Opposite-direction operation stalls pending setup in FunctionFS 4.9. */
	++c->rejected; record(s,"unsupported_or_bad_length_setup_stalled",length);
	return control_io(c,!(r->bRequestType&USB_DIR_IN),data,0) ? 0:1;
}
static int service(const struct Snapshot *s) {
	struct Context *c=s->context;
	if (c->fixture) return 2;
	if (!all_stopped(&c->mtp)) { record(s,"mtp_unexpectedly_resumed",0); return 1; }
	struct pollfd p={c->ep0,POLLIN,0};
	int rc=poll(&p,1,0); if (rc<0 && errno!=EINTR) return 1;
	if (p.revents&POLLIN) {
		struct usb_functionfs_event event;
		ssize_t n=read(c->ep0,&event,sizeof(event));
		if (n!=sizeof(event)) { if (n<0 && (errno==EAGAIN || errno==EIDRM)) return 0; return 1; }
		record(s,"ffs_event",event.type);
		if (event.type==FUNCTIONFS_ENABLE) c->enabled=true;
		if (event.type==FUNCTIONFS_DISABLE || event.type==FUNCTIONFS_UNBIND) c->enabled=false;
		if (event.type==FUNCTIONFS_SETUP && handle_setup(s,&event.u.setup)) { record(s,"setup_io_failed",errno); return 1; }
	}
	if (c->enabled) {
		uint8_t neutral[8]={0}; io_timed_out=0; (void)alarm(1);
		ssize_t n=write(c->epin,neutral,sizeof(neutral)); int saved=errno; (void)alarm(0);
		if (n==sizeof(neutral)) ++c->neutral_reports;
		else if (n<0 && saved!=EAGAIN && saved!=EINTR && saved!=ESHUTDOWN) return 1;
	}
	if (c->completed_at && now_ms()-c->completed_at>=1000) return 2;
	return 0;
}
static void set_paths(struct Context *c,const char *root,const char *mountpoint,const char *name) {
	snprintf(c->name,sizeof(c->name),"%s",name);
	snprintf(c->function,sizeof(c->function),"%s/functions/ffs.%s",root,name);
	snprintf(c->link,sizeof(c->link),"%s/configs/c.1/ffs.%s",root,name);
	snprintf(c->mtp_link,sizeof(c->mtp_link),"%s/configs/c.1/ffs.mtp",root);
	snprintf(c->mtp_function,sizeof(c->mtp_function),"%s/functions/ffs.mtp",root);
	snprintf(c->mountpoint,sizeof(c->mountpoint),"%s",mountpoint);
	const char *attributes[]={"bcdDevice","strings/0x409/product","strings/0x409/serialnumber"};
	for (unsigned i=0;i<3;++i) snprintf(c->attributes[i].path,sizeof(c->attributes[i].path),"%s/%s",root,attributes[i]);
	snprintf(c->attributes[0].lab,sizeof(c->attributes[0].lab),"0x9903");
	snprintf(c->attributes[1].lab,sizeof(c->attributes[1].lab),"NX Controller ep0 lab");
	snprintf(c->attributes[2].lab,sizeof(c->attributes[2].lab),"%s",name);
}
static bool discover_mtp(struct Identity *out) {
	DIR *dir=opendir("/proc"); if (!dir) return false;
	struct dirent *entry; unsigned count=0;
	while ((entry=readdir(dir))) {
		char *end; long pid=strtol(entry->d_name,&end,10); struct Identity id={0};
		if (!*end && pid>1 && !identity((pid_t)pid,&id) && !strcmp(id.exe,MTP_EXE)) { *out=id; ++count; }
	}
	closedir(dir); return count==1 && out->state!='T' && out->state!='t';
}
static bool live_preflight(struct Snapshot *s,struct Context *c) {
	char value[256],adb_link[512];
	if (geteuid() || read_value("/proc/sys/kernel/random/boot_id",value,sizeof(value)) || strcmp(value,AUDITED_BOOT) ||
		read_value("/sys/devices/platform/soc/usbc0/otg_role",value,sizeof(value)) || strcmp(value,"usb_device") ||
		discover_adbd(&s->adb) || !discover_mtp(&c->mtp)) return false;
	snprintf(adb_link,sizeof(adb_link),CONFIG "/ffs.adb");
	if (!link_matches(adb_link,ROOT "/functions/ffs.adb") || !link_matches(c->mtp_link,c->mtp_function) || !allowed_links(c)) return false;
	DIR *dir=opendir(ROOT "/functions"); if (!dir) return false;
	struct dirent *entry; bool safe=true;
	while ((entry=readdir(dir))) if (entry->d_name[0]!='.' && strcmp(entry->d_name,"ffs.adb") && strcmp(entry->d_name,"ffs.mtp")) { safe=false; break; }
	closedir(dir); if (!safe) return false;
	struct stat st;
	if (!lstat(c->link,&st) || errno!=ENOENT || !lstat(c->function,&st) || errno!=ENOENT ||
		!lstat(c->mountpoint,&st) || errno!=ENOENT) return false;
	if (read_value(s->udc_path,s->udc,sizeof(s->udc)) || strcmp(s->udc,"5100000.udc-controller")) return false;
	for (unsigned i=0;i<3;++i) if (read_value(c->attributes[i].path,c->attributes[i].original,sizeof(c->attributes[i].original))) return false;
	ssize_t n=readlink(c->mtp_link,c->mtp_target,sizeof(c->mtp_target)-1);
	if (n<=0 || (size_t)n==sizeof(c->mtp_target)-1) return false;
	c->mtp_target[n]=0;
	return no_sd_regular_fds(&c->mtp) && no_sd_regular_fds(&s->adb);
}
static int control_selftest(void); /* Own fixtures, defined below. */

int main(int argc,char **argv) {
	(void)signal(SIGHUP,SIG_IGN); (void)signal(SIGPIPE,SIG_IGN);
	struct sigaction action={0}; action.sa_handler=request_stop; sigemptyset(&action.sa_mask);
	(void)sigaction(SIGINT,&action,NULL); (void)sigaction(SIGTERM,&action,NULL);
	struct sigaction timeout_action={0}; timeout_action.sa_handler=request_timeout; sigemptyset(&timeout_action.sa_mask);
	(void)sigaction(SIGALRM,&timeout_action,NULL);
	if (argc==2 && !strcmp(argv[1],"--selftest")) return control_selftest();
#ifndef NX_ENABLE_CONTROL_OUT_LIVE
	fputs("Control probe live disabled; run selftest/review first.\n",stderr); return 2;
#endif
	if (argc!=3 || strcmp(argv[1],"--live") || strncmp(argv[2],"/tmp/nx-ep0-",12) ||
		strchr(argv[2]+12,'/') || strstr(argv[2],"..") || strlen(argv[2])>300) return 2;
	int lock=open("/tmp/nx-usb-owner-probe.lock",O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
	if (lock<0 || flock(lock,LOCK_EX|LOCK_NB) || !access("/tmp/nx_usb_mode.lock",F_OK) ||
		!access("/tmp/nx_usb_gamepad_active",F_OK) || !access("/tmp/nx-h2-usb-out.active",F_OK)) return 2;
	struct Context c={.ep0=-1,.epin=-1};
	struct Snapshot s={.log_fd=-1,.unbind=true,.hold_ms=18000,.guardian_ms=23000,.context=&c,
		.before_unbind=before_unbind,.resume_extra=resume_mtp,.activate=activate,.step=service,
		.worker_release=release_endpoints,.cleanup=cleanup};
	char name[80],mountpoint[128];
	snprintf(name,sizeof(name),"nxep0.%ld.%lld",(long)getpid(),(long long)now_ms());
	snprintf(mountpoint,sizeof(mountpoint),"/tmp/%s.ffs",name);
	set_paths(&c,ROOT,mountpoint,name);
	snprintf(s.udc_path,sizeof(s.udc_path),ROOT "/UDC");
	if (!live_preflight(&s,&c)) { fputs("Live preflight refused.\n",stderr); return 2; }
	s.log_fd=open(argv[2],O_CREAT|O_EXCL|O_WRONLY|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600); if (s.log_fd<0) return 2;
	snprintf(s.restore_lock_path,sizeof(s.restore_lock_path),"%s.restore.lock",argv[2]);
	int mutex=open(s.restore_lock_path,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600); if (mutex<0) return 2; close(mutex);
	dprintf(s.log_fd,"snapshot adbd_pid=%ld start=%llu mtp_pid=%ld start=%llu udc=%s lab_serial=%s\n",(long)s.adb.pid,s.adb.start,(long)c.mtp.pid,c.mtp.start,s.udc,name);
	dprintf(s.log_fd,"snapshot_boot=%s mtp_link=%s\n",AUDITED_BOOT,c.mtp_target);
	for (unsigned i=0;i<3;++i) dprintf(s.log_fd,"snapshot_attribute path=%s value=%s\n",c.attributes[i].path,c.attributes[i].original);
	printf("lab_serial=%s journal=%s\n",name,argv[2]); fflush(stdout);
	int rc=run_probe(&s);
	int lifecycle_rc=rc;
	if (c.mask!=3) rc=1;
	dprintf(s.log_fd,"output_mask=%u rejected=%u neutral_reports=%u lifecycle_rc=%d probe_rc=%d\n",c.mask,c.rejected,c.neutral_reports,lifecycle_rc,rc);
	close(s.log_fd); close(lock); /* Journal and mutex retained as evidence. */
	return rc;
}

static int create_file(const char *path,const char *value) {
	int fd=open(path,O_CREAT|O_EXCL|O_WRONLY,0600); if (fd<0) return -1; close(fd);
	return write_value(path,value);
}
static bool absent(const char *path) { struct stat st; return lstat(path,&st)<0 && errno==ENOENT; }
static int control_selftest(void) {
	unsigned failed=0;
	uint8_t data[32]; for (unsigned phase=1;phase<=2;++phase) {
		for (size_t i=0;i<32;++i) data[i]=(uint8_t)((phase==1?0xa0:0xb0)+(i&15));
		if (payload_phase(data,32)!=phase || payload_phase(data,31)) ++failed;
		data[31]^=1; if (payload_phase(data,32)) ++failed;
	}
	struct usb_ctrlrequest setup={.bRequestType=0x21,.bRequest=HID_REQ_SET_REPORT,.wValue=0x0200,.wIndex=0,.wLength=32};
	if (!output_setup(&setup)) ++failed;
	setup.wLength=33; if (output_setup(&setup)) ++failed;
	setup.wLength=32; setup.wIndex=1; if (output_setup(&setup)) ++failed;
	setup.wIndex=0; setup.wValue=0x0300; if (output_setup(&setup)) ++failed;
	setup.wValue=0x0201; if (output_setup(&setup)) ++failed;
	setup.wValue=0x0200; setup.bRequestType=0xa1; if (output_setup(&setup)) ++failed;
	if (descriptors.fs_interface.bNumEndpoints!=1 || descriptors.hs_interface.bNumEndpoints!=1 ||
		descriptors.fs_ep.bInterval!=4 || descriptors.hs_ep.bInterval!=6) ++failed;
	printf("PACKET_DESCRIPTOR_TEST failed=%u\n",failed);
	char base[128]; snprintf(base,sizeof(base),"/tmp/nx-ep0-selftest.%ld",(long)getpid());
	if (mkdir(base,0700)) return 2;
	const char *dirs[]={"functions","functions/ffs.adb","functions/ffs.mtp","configs","configs/c.1","strings","strings/0x409"};
	for (unsigned i=0;i<7;++i) { char path[256]; snprintf(path,sizeof(path),"%s/%s",base,dirs[i]); if (mkdir(path,0700)) return 2; }
	struct Context c={.ep0=-1,.epin=-1,.fixture=true};
	char mountpoint[256]; snprintf(mountpoint,sizeof(mountpoint),"%s/mount",base);
	set_paths(&c,base,mountpoint,"fixture");
	snprintf(c.mtp_target,sizeof(c.mtp_target),"../../functions/ffs.mtp");
	if (symlink(c.mtp_target,c.mtp_link)) return 2;
	char adb_link[256]; snprintf(adb_link,sizeof(adb_link),"%s/configs/c.1/ffs.adb",base);
	if (symlink("../../functions/ffs.adb",adb_link)) return 2;
	struct Snapshot s={.log_fd=STDOUT_FILENO,.unbind=true,.hold_ms=30,.context=&c,
		.before_unbind=before_unbind,.resume_extra=resume_mtp,.activate=activate,.step=service,
		.worker_release=release_endpoints,.cleanup=cleanup};
	snprintf(s.udc_path,sizeof(s.udc_path),"%s/UDC",base); snprintf(s.udc,sizeof(s.udc),"fixture.udc");
	if (create_file(s.udc_path,s.udc)) return 2;
	snprintf(s.restore_lock_path,sizeof(s.restore_lock_path),"%s/restore.lock",base);
	if (create_file(s.restore_lock_path,"")) return 2;
	for (unsigned i=0;i<3;++i) { snprintf(c.attributes[i].original,sizeof(c.attributes[i].original),"original-%u",i); if (create_file(c.attributes[i].path,c.attributes[i].original)) return 2; }
	pid_t adb=fork(); if (adb<0) return 2; if (!adb) { for (;;) pause(); }
	pid_t mtp=fork(); if (mtp<0) return 2; if (!mtp) { for (;;) pause(); }
	if (identity(adb,&s.adb) || identity(mtp,&c.mtp)) return 2;
	for (unsigned test=0;test<12;++test) {
		c.fault_at=test>=1 && test<=7 ? test:0;
		s.fault=test==8 ? WORKER_AFTER_STOP : test==9 ? WORKER_AFTER_UNBIND : test==10 ? GUARD_AFTER_STOP : test==11 ? GUARD_AFTER_UNBIND : NO_FAULT;
		pid_t worker=fork(); if (worker<0) return 2;
		if (!worker) _exit(run_probe(&s));
		int status; (void)waitpid(worker,&status,0); pause_ms(250);
		char value[256]; struct Identity a={0},m={0};
		bool ok=!read_value(s.udc_path,value,sizeof(value)) && !strcmp(value,s.udc) &&
			!identity(adb,&a) && a.start==s.adb.start && a.state!='T' && a.state!='t' &&
			!identity(mtp,&m) && m.start==c.mtp.start && m.state!='T' && m.state!='t' &&
			link_matches(c.mtp_link,c.mtp_function) && absent(c.link) && absent(c.function) && absent(c.mountpoint);
		for (unsigned i=0;i<3;++i) ok=ok && !read_value(c.attributes[i].path,value,sizeof(value)) && !strcmp(value,c.attributes[i].original);
		bool expected=test==0 ? WIFEXITED(status) && !WEXITSTATUS(status) :
			test<=9 ? WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL : WIFEXITED(status) && WEXITSTATUS(status)==1;
		printf("CONTROL_LIFECYCLE_TEST case=%u restored=%d expected_exit=%d\n",test,ok,expected); if (!ok || !expected) ++failed;
	}
	(void)kill(adb,SIGCONT); (void)kill(mtp,SIGCONT); (void)kill(adb,SIGKILL); (void)kill(mtp,SIGKILL);
	(void)waitpid(adb,NULL,0); (void)waitpid(mtp,NULL,0);
	(void)unlink(adb_link); (void)unlink(c.mtp_link); (void)unlink(s.udc_path); (void)unlink(s.restore_lock_path);
	for (unsigned i=0;i<3;++i) (void)unlink(c.attributes[i].path);
	for (unsigned i=7;i>0;--i) { char path[256]; snprintf(path,sizeof(path),"%s/%s",base,dirs[i-1]); (void)rmdir(path); }
	(void)rmdir(base);
	printf("CONTROL_SELFTEST failed=%u (own files/processes only)\n",failed);
	return failed?1:0;
}
