#include "taskmonitordebugfs.h"static int target = 0;module_param(target, int, 0660);static struct task_monitor *monitored = NULL;static struct task_struct *monitor_thread;static unsigned interval = 5; /* interval between reports in seconds */static unsigned frequency = HZ; /* samples frequency */static mempool_t * memp;static int elem_to_keep = 5;static struct kmem_cache * task_sample_cache = NULL;	//KMEM_CACHE(task_sample, SLAB_PANIC | SLAB_NOTRACK);static struct shrinker sh = {	.count_objects = task_count_objects,	.scan_objects = task_scan_objects,	.seeks = DEFAULT_SEEKS,	.batch = 0,	.flags = 0};static struct kobj_attribute kobj_attr = __ATTR_RO(taskmonitor);static struct file_operations ct_file_ops = {        .owner   = THIS_MODULE,        .open    = debugfs_open,        .read    = seq_read,        .llseek  = seq_lseek,        .release = seq_release};static struct dentry *debug_file;static int debug_show(struct seq_file *m, void *p){	// 50 by line	// Max total PAGE_SIZE / 50	// list_reverse	int nb_max_sample = PAGE_SIZE / 50;	int i = 0;	int offset = 0;	struct list_head * tmp;	struct list_head * tmp_scie;	mutex_lock(&(monitored->m));	list_for_each_safe(tmp_scie, tmp, &(monitored->head)) {		struct task_sample *proc = container_of(tmp_scie, struct task_sample, list);		kref_get(&proc->ref);		if (i < nb_max_sample){	       		seq_printf(m, "pid %d : %llu usr %llu sys \n", 				target, proc->utime, proc->stime);		}		kref_put(&proc->ref, free_task_sample);			}	mutex_unlock(&(monitored->m));	return offset;}static int debugfs_open(struct inode *inode, struct file *file){	return single_open(file, debug_show, NULL);}static ssize_t taskmonitor_show(struct kobject *kobj,		struct kobj_attribute *attr, char *buf){	// 50 by line	// Max total PAGE_SIZE / 50	// list_reverse	int nb_max_sample = PAGE_SIZE / 50;	int i = 0;	int offset = 0;	struct list_head * tmp;	struct list_head * tmp_scie;	mutex_lock(&(monitored->m));	list_for_each_safe(tmp_scie, tmp, &(monitored->head)) {		struct task_sample *proc = container_of(tmp_scie, struct task_sample, list);		kref_get(&proc->ref);		if (i < nb_max_sample){			offset += scnprintf(buf+offset, 50, "pid %d : %llu usr %llu sys \n", 				target, proc->utime, proc->stime);		}		kref_put(&proc->ref, free_task_sample);			}	mutex_unlock(&(monitored->m));	return offset;}static ssize_t taskmonitor_store(struct kobject *kobj,		struct kobj_attribute *attr, const char *buf,			size_t count){	pr_warn("Taskmonitor store\n");	if (monitor_thread != NULL && strncmp(buf, "stop", 4) == 0) {		pr_warn("Taskmonitor store stop\n");		kthread_stop(monitor_thread);		monitor_thread = NULL;	} else if (monitor_thread == NULL && strncmp(buf, "start", 5) == 0) {		pr_warn("Taskmonitor store start\n");		monitor_thread = kthread_run(monitor_fn, NULL, "monitor_fn");	}	return count;}static void free_task_sample(struct kref *kref){	struct task_sample *task = container_of(kref, struct task_sample, ref);	//kmem_cache_free(task_sample_cache, task);	mempool_free(task, memp);}static unsigned long task_scan_objects(struct shrinker * sh, struct shrink_control *sc){	struct list_head *tmp;	struct list_head *tmp_scie;	int nb_free=0;	pr_warn("scan func \n");	pr_warn("Kern need %lu to free \n", sc->nr_to_scan);	mutex_lock(&(monitored->m));	list_for_each_safe(tmp_scie, tmp,&(monitored->head)) {		struct task_sample *proc = container_of(tmp_scie, struct task_sample, list);		kref_get(&proc->ref);		list_del_init(tmp_scie);		nb_free++;		monitored->nb_samples--;		kref_put(&proc->ref, free_task_sample);		if(nb_free >= sc->nr_to_scan)			break;					}	mutex_unlock(&(monitored->m));	sc->nr_scanned = nb_free;	return nb_free;}static unsigned long task_count_objects(struct shrinker * sh, struct shrink_control *sc){	return (monitored->nb_samples > elem_to_keep)?(monitored->nb_samples - elem_to_keep):0;}static bool get_sample(struct task_sample *sample){	if (pid_alive(monitored->proc) == 1) {		sample->utime = monitored->proc->utime;		sample->stime = monitored->proc->stime;		kref_init(&sample->ref);		return 1;	}	return 0;}static bool save_sample(void){	//struct task_sample * task_s = kmalloc(sizeof(*task_s), GFP_KERNEL);	//struct task_sample * task_s = kmem_cache_alloc(task_sample_cache, GFP_KERNEL);	struct task_sample * task_s = mempool_alloc(memp, GFP_KERNEL);	if(task_s != NULL && get_sample(task_s)){		mutex_lock(&(monitored->m));		list_add_tail((&(task_s->list)), (&(monitored->head)));		mutex_unlock(&(monitored->m));		monitored->nb_samples++;		pr_warn("pid %d : %llu usr %llu sys (n°%d size = %lu)\n", target,			task_s->utime, task_s->stime, monitored->nb_samples, sizeof(*task_s));		return 0;	}		return 1;}static int monitor_fn(void *data){	//struct task_sample sample;	int n = 0;	pr_info("Thread starting.\n");	while (!kthread_should_stop()) {		if (++n % (interval * (HZ / frequency)) == 0) {			if(save_sample()){				pr_warn("Thread fault.\n");				break;			}		}					set_current_state(TASK_UNINTERRUPTIBLE);		schedule_timeout(frequency);	}	return 0;}static int monitor_pid(pid_t pid){	struct pid *pid_found = find_get_pid(pid);	if (pid_found == NULL) {		pr_warn("PID not found\n");		return -1;	}	monitored = kmalloc(sizeof(*monitored), GFP_KERNEL);	monitored->pid = pid_found;	INIT_LIST_HEAD(&(monitored->head));	monitored->nb_samples = 0;	mutex_init(&(monitored->m));	monitored->proc = get_pid_task(monitored->pid, PIDTYPE_PID);	return 0;}static int __init taskmonitor_init(void){	pr_info("Hello, taskmonitor\n");	if (monitor_pid(target))		goto ERROR1;	if (sysfs_create_file(kernel_kobj, &(kobj_attr.attr)))		goto ERROR2;	task_sample_cache = KMEM_CACHE(task_sample, SLAB_PANIC);	memp = mempool_create_slab_pool(elem_to_keep, task_sample_cache);	if (!memp)		goto ERROR3;	if (register_shrinker(&sh))		goto ERROR4;	debug_file = debugfs_create_file("taskmonitordebug", S_IRUGO,				       NULL, NULL, &ct_file_ops);	if (!debug_file)		goto ERROR5;	monitor_thread = kthread_run(monitor_fn, NULL, "monitor_fn");	pr_info("Module taskmonitor successfully loaded.\n");	return 0;ERROR5:	unregister_shrinker(&sh);ERROR4:	mempool_destroy(memp);ERROR3:	sysfs_remove_file(kernel_kobj, &(kobj_attr.attr));ERROR2:	free_monitored();ERROR1:	pr_info("Module taskmonitor failed to load.\n");	return -1;}static void __exit taskmonitor_exit(void){	struct list_head * tmp;	struct list_head * tmp_scie;		pr_info("Bye, taskmonitor\n");	if (monitor_thread)		kthread_stop(monitor_thread);	if (monitored != NULL) {		mutex_lock(&(monitored->m));		list_for_each_safe(tmp_scie, tmp,&(monitored->head)) {			struct task_sample * proc = container_of(tmp_scie, struct task_sample, list);			list_del_init(tmp_scie);			//kfree(proc);			//kmem_cache_free(task_sample_cache, proc);			monitored->nb_samples--;			kref_put(&proc->ref, free_task_sample);					}		mutex_unlock(&(monitored->m));	}	free_monitored();	kmem_cache_destroy(task_sample_cache);		mempool_destroy(memp);	unregister_shrinker(&sh);	sysfs_remove_file(kernel_kobj, &(kobj_attr.attr));	debugfs_remove(debug_file);}static void free_monitored(void){	if (!monitored)		return;	put_task_struct(monitored->proc);	put_pid(monitored->pid);	kfree(monitored);}