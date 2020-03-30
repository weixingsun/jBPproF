#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <math.h>
#include <map>
#include <stack>
#include <iostream>
#include <sstream>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <bcc/BPF.h>
#include <jvmti.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>

using namespace std;

#define MAX_STACK_DEPTH 128
static bool writing_perf = false;
static FILE* out_cpu;
static FILE* out_thread;
//static FILE* out_mem;
static FILE* out_perf;
static jlong start_time;
static jrawMonitorID vmtrace_lock;
static jvmtiEnv* jvmti = NULL;
static jrawMonitorID tree_lock;

static int SAMPLE_TOP_N = 20;
static int COUNT_TOP_N = 0;
static int LAT_TOP_N = 0;
static int BPF_PERF_FREQ = 49;
static int DURATION = 10;
static int MON_DURATION = 5;
static int BP_SEQ = 1;
static ebpf::BPF bpf;
static map<int,string> BPF_TXT_MAP;
static map<int,string> BPF_FN_MAP;

struct Frame {
    jlong samples;
    jlong bytes;
    map<jmethodID, Frame> children;
};
static map<string, Frame> root;

struct method_type {
    uint64_t    addr;
    uint64_t    ret;
    string      name;
    bool operator<(const method_type &m) const{
        return addr < m.addr;
    }
};
//for flame generation
struct stack_key_t {
  int pid;
  uint64_t kernel_ip;
  uint64_t kernel_ret_ip;
  int user_stack_id;
  int kernel_stack_id;
  char name[16];
};
//for top method 
struct method_key_t {
  int pid;
  uint64_t kernel_ip;
  int user_stack_id;
  int kernel_stack_id;
  uint64_t bp;
  uint64_t ret;
};
//for method latency
struct hist_key_t {
    uint64_t key;
    uint64_t slot;
};
//for thread sampling
struct thread_key_t {
    int pid;
    int tid;
    long state;
    char name[16];
};

string BPF_TXT_TRD = R"(
#include <linux/sched.h>
#include <uapi/linux/ptrace.h>
#include <uapi/linux/bpf_perf_event.h>
struct thread_key_t {
    u32 pid;
    u32 tid;
    long state;
    char name[TASK_COMM_LEN];
};
BPF_HASH(counts, struct thread_key_t);
int do_perf_event_thread(struct bpf_perf_event_data *ctx) {
    u64 id = bpf_get_current_pid_tgid();
    u32 tgid = id >> 32;
    u32 pid = id;
    if (pid == 0) return 0;
    if (!PID) return 0;
    struct thread_key_t key = {.pid = tgid};
    key.tid = pid;
    key.state = 0;    //must be runnable
    bpf_get_current_comm(&key.name, sizeof(key.name));
    counts.increment(key);
    return 0;
}
)";
///////////////////////////////////////////
string BPF_TXT_FLM = R"(
#include <linux/sched.h>
#include <uapi/linux/ptrace.h>
#include <uapi/linux/bpf_perf_event.h>
struct stack_key_t {
    u32 pid;
    u64 kernel_ip;
    u64 kernel_ret_ip;
    int user_stack_id;
    int kernel_stack_id;
    char name[TASK_COMM_LEN];
};
BPF_HASH(counts, struct stack_key_t);
BPF_STACK_TRACE(stack_traces, 16384); //STACK_SIZE
int do_perf_event_flame(struct bpf_perf_event_data *ctx) {
    u64 id = bpf_get_current_pid_tgid();
    u32 tgid = id >> 32;
    u32 pid = id;
    if (pid == 0) return 0;
    if (!PID) return 0;
    struct stack_key_t key = {.pid = tgid};
    bpf_get_current_comm(&key.name, sizeof(key.name));
    key.user_stack_id = stack_traces.get_stackid(&ctx->regs, BPF_F_USER_STACK);
    key.kernel_stack_id = stack_traces.get_stackid(&ctx->regs, 0);
    if (key.kernel_stack_id >= 0) {
        // populate extras to fix the kernel stack
        u64 ip = PT_REGS_IP(&ctx->regs);
        u64 page_offset;
        // if ip isn't sane, leave key ips as zero for later checking
#if defined(CONFIG_X86_64) && defined(__PAGE_OFFSET_BASE)
        // x64, 4.16, ..., 4.11, etc., but some earlier kernel didn't have it
        page_offset = __PAGE_OFFSET_BASE;
#elif defined(CONFIG_X86_64) && defined(__PAGE_OFFSET_BASE_L4)
        // x64, 4.17, and later
#if defined(CONFIG_DYNAMIC_MEMORY_LAYOUT) && defined(CONFIG_X86_5LEVEL)
        page_offset = __PAGE_OFFSET_BASE_L5;
#else
        page_offset = __PAGE_OFFSET_BASE_L4;
#endif
#else
        // earlier x86_64 kernels, e.g., 4.6, comes here
        // arm64, s390, powerpc, x86_32
        page_offset = PAGE_OFFSET;
#endif
        if (ip > page_offset) {
            key.kernel_ip = ip;
        }
    }
    counts.increment(key);
    return 0;
}
)";
string BPF_TXT_MTD = R"(
#include <linux/sched.h>
#include <uapi/linux/ptrace.h>
#include <uapi/linux/bpf_perf_event.h>
struct method_key_t {
    u32 pid;
    u64 kernel_ip;
    int user_stack_id;
    int kernel_stack_id;
    u64 bp;
    u64 ret;
};
typedef struct hist_key {
    u64 key;
    u64 count;
} hist_key_t;
BPF_HASH(counts, struct method_key_t);
BPF_STACK_TRACE(stack_traces, 16384);
BPF_TABLE("array", int, int, top_counter, 16);
BPF_TABLE("array", int, int, top_ret_counter, 16);
BPF_HASH(start, u32);
BPF_HISTOGRAM(dist, u64);
static int inc(int idx) {
    int *ptr = top_counter.lookup(&idx);
    if (ptr) ++(*ptr);
    return 0;
}
static int incr(int idx) {
    int *ptr = top_ret_counter.lookup(&idx);
    if (ptr) ++(*ptr);
    return 0;
}
//struct bpf_perf_event_data *ctx
int do_bp_count0(void *ctx){ return inc(0);}
int do_bp_count1(void *ctx){ return inc(1);}
int do_bp_count2(void *ctx){ return inc(2);}
int do_bp_count3(void *ctx){ return inc(3);}
int do_bp_count4(void *ctx){ return inc(4);}
int do_ret_count0(void *ctx){ return incr(0);}
int do_ret_count1(void *ctx){ return incr(1);}
int do_ret_count2(void *ctx){ return incr(2);}
int do_ret_count3(void *ctx){ return incr(3);}
int do_ret_count4(void *ctx){ return incr(4);}
int func_entry0(struct bpf_perf_event_data *ctx){
    u64 pid_tgid = bpf_get_current_pid_tgid();
    //u32 tgid = pid_tgid >> 32;
    u32 pid = pid_tgid;
    //u64 ip = PT_REGS_IP(&ctx->regs);
    //ipaddr.update(&pid, &ip);
    u64 ts = bpf_ktime_get_ns();
    start.update(&pid, &ts);
    return 0;
}
int func_return0(struct bpf_perf_event_data *ctx) {
    u64 pid_tgid = bpf_get_current_pid_tgid();
    //u32 tgid = pid_tgid >> 32;
    u32 pid = pid_tgid;
    u64 *tsp = start.lookup(&pid);
    if (tsp == 0) return 0;
    u64 delta = bpf_ktime_get_ns() - *tsp;
    start.delete(&pid);
    u64 key = bpf_log2l(delta);
    dist.increment(key);
    return 0;
} 
int do_perf_event_method(struct bpf_perf_event_data *ctx) {
    u64 id = bpf_get_current_pid_tgid();
    u32 tgid = id >> 32;
    u32 pid = id;
    if (pid == 0) return 0;
    if (!PID) return 0;
    //struct task_struct *p = (struct task_struct*) bpf_get_current_task();
    //void* ptr = p->stack + THREAD_SIZE - TOP_OF_KERNEL_STACK_PADDING;
    //struct pt_regs* regs = ((struct pt_regs *)ptr) - 1;
    struct pt_regs* regs = &ctx->regs;
    struct method_key_t key = {.pid = tgid};
    key.bp = regs->bp;
    //key.ret = regs->r14;
    u64 ret = regs->bp+8;
    bpf_probe_read(&key.ret, sizeof(u64), (void *)ret);
    key.user_stack_id = stack_traces.get_stackid(&ctx->regs, BPF_F_USER_STACK);
    key.kernel_stack_id = stack_traces.get_stackid(&ctx->regs, 0);
    if (key.kernel_stack_id >= 0) {
        // populate extras to fix the kernel stack
        u64 ip = PT_REGS_IP(&ctx->regs);
        u64 page_offset;
        // if ip isn't sane, leave key ips as zero for later checking
#if defined(CONFIG_X86_64) && defined(__PAGE_OFFSET_BASE)
        // x64, 4.16, ..., 4.11, etc., but some earlier kernel didn't have it
        page_offset = __PAGE_OFFSET_BASE;
#elif defined(CONFIG_X86_64) && defined(__PAGE_OFFSET_BASE_L4)
        // x64, 4.17, and later
#if defined(CONFIG_DYNAMIC_MEMORY_LAYOUT) && defined(CONFIG_X86_5LEVEL)
        page_offset = __PAGE_OFFSET_BASE_L5;
#else
        page_offset = __PAGE_OFFSET_BASE_L4;
#endif
#else
        // earlier x86_64 kernels, e.g., 4.6, comes here
        // arm64, s390, powerpc, x86_32
        page_offset = PAGE_OFFSET;
#endif
        if (ip > page_offset) {
            key.kernel_ip = ip;
        }
    }
    counts.increment(key);
    return 0;
}
)";
void genbpftextmap(){
    BPF_TXT_MAP[0]=BPF_TXT_FLM;
    BPF_TXT_MAP[1]=BPF_TXT_MTD;
    BPF_TXT_MAP[2]=BPF_TXT_TRD;
    BPF_FN_MAP[0]="do_perf_event_flame";
    BPF_FN_MAP[1]="do_perf_event_method";
    BPF_FN_MAP[2]="do_perf_event_thread";
}
string get_prof_func(int id){
    return BPF_FN_MAP[id];
}
string getbpftext(int id){
    return BPF_TXT_MAP[id];
}

static jlong get_time(jvmtiEnv* jvmti) {
    jlong current_time;
    jvmti->GetTime(&current_time);
    return current_time;
}

// Converts JVM internal class signature to human readable name
static string decode_class_signature(char* class_sig) {
    //Lorg/net/XX  [I
    switch (class_sig[1]) {
        case 'B': return "byte";
        case 'C': return "char";
        case 'D': return "double";
        case 'F': return "float";
        case 'I': return "int";
        case 'J': return "long";
        case 'S': return "short";
        case 'Z': return "boolean";
    }
    // rm first 'L'|'[' and last ';'
    class_sig++;
    class_sig[strlen(class_sig) - 1] = 0;
    // Replace '/' with '.'
    for (char* c = class_sig; *c; c++) {
        if (*c == '/') *c = '.';
    }

    return class_sig;
}
void jvmti_free(char* ptr){
    if (ptr != NULL) jvmti->Deallocate((unsigned char*) ptr);
}
static string get_method_name(jmethodID method) {
    jclass method_class;
    char* class_sig = NULL;
    char* method_name = NULL;
    string result;
    if (jvmti->GetMethodDeclaringClass(method, &method_class) == 0 &&
        jvmti->GetClassSignature(method_class, &class_sig, NULL) == 0 &&
        jvmti->GetMethodName(method, &method_name, NULL, NULL) == 0) {
        result.assign(decode_class_signature(class_sig) + "." + method_name);
        jvmti_free(method_name);
        jvmti_free(class_sig);
	return result;
    } else {
        return "(NONAME)";
    }
}

static void write_line(const string stack_line, const string& class_name, const Frame* f) {
    if (f->samples > 0) {
        //fprintf(out_mem, "%s %s_[%ld] \n", stack_line.c_str(), class_name.c_str(), f->samples);
    }
    for (auto it = f->children.begin(); it != f->children.end(); ++it) {
        write_line(stack_line + get_method_name(it->first) + ";", class_name, &it->second);
    }
}

static void write_loop_root() {
    for (auto it = root.begin(); it != root.end(); ++it) {
        write_line("", it->first, &it->second);
    }
}

static void record_stack_trace(char* class_sig, jvmtiFrameInfo* frames, jint count, jlong size) {
    Frame* f = &root[decode_class_signature(class_sig)];
    while (--count >= 0) {
        f = &f->children[frames[count].method];
    }
    f->samples++;
    f->bytes += size;
}

void JNICALL SampledObjectAlloc(jvmtiEnv* jvmti, JNIEnv* env, jthread thread,
                                jobject object, jclass object_klass, jlong size) {
    jvmtiFrameInfo frames[MAX_STACK_DEPTH];
    jint count;
    if (jvmti->GetStackTrace(thread, 0, MAX_STACK_DEPTH, frames, &count) != 0) {
        return;
    }

    char* class_sig;
    if (jvmti->GetClassSignature(object_klass, &class_sig, NULL) != 0) {
        return;
    }
    jvmti->RawMonitorEnter(tree_lock);
    record_stack_trace(class_sig, frames, count, size);
    jvmti->RawMonitorExit(tree_lock);

    jvmti_free(class_sig);
}

void JNICALL DataDumpRequest(jvmtiEnv* jvmti) {
    jvmti->RawMonitorEnter(tree_lock);
    write_loop_root();
    jvmti->RawMonitorExit(tree_lock);
}

void JNICALL VMDeath(jvmtiEnv* jvmti, JNIEnv* env) {
    DataDumpRequest(jvmti);
}

void JNICALL GarbageCollectionStart(jvmtiEnv *jvmti) {
    DataDumpRequest(jvmti);
}

void JNICALL GarbageCollectionFinish(jvmtiEnv *jvmti_env) {
    DataDumpRequest(jvmti);
    //trace(jvmti_env, "GC finished");
}
void JNICALL CompiledMethodLoad(jvmtiEnv *jvmti, jmethodID method, jint code_size,
		const void* code_addr, jint map_length, const jvmtiAddrLocationMap* map, const void* compile_info){
    if (out_perf!=NULL && !writing_perf){
	string method_name = get_method_name(method);
	//fprintf(stdout, "method_name:  %s \n", method_name );
	fprintf(out_perf, "%lx %x %s\n", (unsigned long) code_addr, code_size, method_name.c_str());
    }
}
void JNICALL CompiledMethodUnload(jvmtiEnv *jvmti_env, jmethodID method, const void* code_addr){

}
void JNICALL DynamicCodeGenerated(jvmtiEnv *jvmti, const char* method_name, const void* code_addr, jint code_size) {
    if (out_perf!=NULL && !writing_perf){
        fprintf(out_perf, "%lx %x %s\n", (unsigned long) code_addr, code_size, method_name);
    }
}
/////////////////////////////////////////////////////////////////////////
bool str_replace(string& str, const string& from, const string& to) {
    size_t start_pos = str.find(from);
    if(start_pos == std::string::npos)
        return false;
    str.replace(start_pos, from.length(), to);
    return true;
}

perf_event_attr AttachBreakPoint(uint64_t addr, const string& fn, int seq){
    string fn_seq = fn+to_string(seq);
    fprintf(stdout, "attach to breakpoint: addr=%lx fn=%s \n", addr, fn_seq.c_str() );
    struct perf_event_attr attr = {};
    attr.type = PERF_TYPE_BREAKPOINT;
    attr.bp_len = HW_BREAKPOINT_LEN_8; //for exec
    attr.size = sizeof(struct perf_event_attr);
    attr.bp_addr = addr;
    //attr.config = 0;
    attr.config = BP_SEQ;
    BP_SEQ++;
    attr.bp_type = HW_BREAKPOINT_EMPTY;
    attr.bp_type |= HW_BREAKPOINT_X;  //HW_BREAKPOINT_R/HW_BREAKPOINT_W conflict with HW_BREAKPOINT_X
    attr.sample_period = 1;    // Trigger for every event
    attr.precise_ip = 2;        //request sync delivery
    attr.wakeup_events = 1;
    //attr.inherit = 1;
    int pid=-1;
    int cpu=-1;
    int group=-1;
    auto att_r = bpf.attach_perf_event_raw(&attr,fn_seq,pid,cpu,group);
    if(att_r.code()!=0) cerr << att_r.msg() << endl;
    return attr;
}

void StartBPF(int id) {
    //cout << "StartBPF(" << id << ")" << endl;
    //ebpf::BPF bpf;
    int pid = getpid();
    const string PID = "(tgid=="+to_string(pid)+")";
    string BPF_TXT = getbpftext(id);
    str_replace(BPF_TXT, "PID", PID);
    cout << "BPF:" << endl << BPF_TXT << endl;
    auto init_r = bpf.init(BPF_TXT);
    if (init_r.code() != 0) {
        cerr << init_r.msg() << endl;
    }
    int pid2=-1;
    string fn = get_prof_func(id);
    auto att_r = bpf.attach_perf_event(PERF_TYPE_SOFTWARE, PERF_COUNT_HW_CPU_CYCLES, fn, BPF_PERF_FREQ, 0, pid2);
    if (att_r.code() != 0) {
        cerr << "failed to attach fn:" << fn <<  " pid:" << pid << " err:" << att_r.msg() << endl;
    }else{
        cout << "attached fn:"<<fn <<" to pid:" << pid << " perf event "<< endl;
    }
    cout << "BPF sampling " << DURATION << " seconds" << endl;
}
void StopBPF(){
    sleep(DURATION);
    if (out_perf!=NULL){
        writing_perf=true;
        fclose(out_perf);
        writing_perf=false;
        out_perf=NULL;
    }
    bpf.detach_perf_event(PERF_TYPE_SOFTWARE, PERF_COUNT_HW_CPU_CYCLES);
}
void PrintThread(){
    auto table = bpf.get_hash_table<thread_key_t, uint64_t>("counts").get_table_offline();
    /*
    sort( table.begin(), table.end(),
      [](pair<thread_key_t, uint64_t> a, pair<thread_key_t, uint64_t> b) {
        return a.second < b.second;
      }
    );
    */
    //-1 unrunnable, 0 runnable, >0 stopped
    int total_samples = 0;
    fprintf(out_thread, "pid\ttid\tcount\tpct\tname\n");
    for (auto it : table) {
	total_samples+=it.second;
    }
    for (auto it : table) {
	fprintf(out_thread, "%d\t%d\t%ld\t%0.2f\t%s\n", it.first.pid,it.first.tid,it.second, (float)100*it.second/total_samples, it.first.name );
    }

    fclose(out_thread);
}
template <typename A, typename B> multimap<B, A> flip_map(map<A,B> & src) {
    multimap<B,A> dst;
    for(typename map<A, B>::const_iterator it = src.begin(); it != src.end(); ++it)
        dst.insert(pair<B, A>(it->second, it->first));
    return dst;
}
void PrintTopMethodCount(method_type methods[], int n){
    //auto cnt = bpf.get_array_table<unsigned long long>("top_counter");
    ebpf::BPFArrayTable<int> cnt = bpf.get_array_table<int>("top_counter");
    ebpf::StatusTuple res(0);
    fprintf(out_cpu, "Monitoring Methods:\ncount\t method_addr\t method_name\n");
    for (int i=0; i<n; i++){
        if (i>n+1) break;
        int value;
        res = cnt.get_value(i, value);
        if (res.code()!=0) cerr<<res.msg()<<endl;
        else fprintf(out_cpu, "%d\t %lx\t %s\n", value, methods[i].addr, methods[i].name.c_str() );
    }
}
void PrintTopMethodRetCount(method_type methods[], int n){
    ebpf::BPFArrayTable<int> cnt = bpf.get_array_table<int>("top_ret_counter");
    ebpf::StatusTuple res(0);
    fprintf(out_cpu, "Monitoring Methods:\ncount\t method_ret_addr\t method_name\n");
    for (int i=0; i<n; i++){
        if (i>n+1) break;
        int value;
        res = cnt.get_value(i, value);
	if (res.code()!=0) cerr<<res.msg()<<endl;
	else fprintf(out_cpu, "%d\t %lx\t %s\n", value, methods[i].ret, methods[i].name.c_str() );
    }
}
void DetachBreakPoint(struct perf_event_attr* attr){
    cout<<"detached bp"<<endl;
    bpf.detach_perf_event_raw(attr);
}
void CountMethods(method_type methods[], int n){
    perf_event_attr peas[2*n];
    for (int i=0; i<n; i++){
        if (i>n-1) break;
	int j=2*i;
        peas[j]  =AttachBreakPoint(methods[i].addr, "do_bp_count", i);
        peas[j+1]=AttachBreakPoint(methods[i].ret, "do_ret_count", i);
    }
    cout<<"counting "<< n <<" methods for "<<MON_DURATION<<" second"<<endl;
    sleep(MON_DURATION);

    for (perf_event_attr attr : peas){
        DetachBreakPoint(&attr);
    }
    PrintTopMethodCount(methods,n);
    PrintTopMethodRetCount(methods,n);
}
void LatencyMethod(method_type method){
    perf_event_attr peas[2];
    peas[0]  =AttachBreakPoint(method.addr, "func_entry", 0);
    peas[1]=AttachBreakPoint(method.ret, "func_return", 0);
    cout<<"latency measuring for "<<MON_DURATION<<" second"<<endl;
    sleep(MON_DURATION);
    DetachBreakPoint(&peas[0]);
    DetachBreakPoint(&peas[1]);
    auto dist = bpf.get_hash_table<uint64_t, uint64_t>("dist").get_table_offline();
    sort( dist.begin(), dist.end(),
      [](pair<uint64_t, uint64_t> a, pair<uint64_t, uint64_t> b) {
        return a.first < b.first;
      }
    );
    fprintf(out_cpu, "\n(%ld) latency for method: (%lx -> %lx)\t\"%s\"\n", dist.size(), method.addr, method.ret, method.name.c_str() );
    fprintf(out_cpu, "nsecs    \t count\n"); // distribution\n");
    for (auto it=dist.begin(); it!=dist.end();it++) {
        fprintf(out_cpu, ">%ld     \t %ld\t \n", (long)exp2(it->first), it->second );
    }
}

void PrintTopMethods(int n){
    auto table = bpf.get_hash_table<method_key_t, uint64_t>("counts").get_table_offline();
    auto stacks = bpf.get_stack_table("stack_traces");
    cout<<"sampled "<< table.size() << " methods"<<endl;
    sort( table.begin(), table.end(),
      [](pair<method_key_t, uint64_t> a, pair<method_key_t, uint64_t> b) {
        return a.second > b.second;
      }
    );
    fprintf(stdout, "count \t bp     \t ret    \t addr       \t name\n");
    map<method_type, int> mout;
    for (auto it : table) {
        uint64_t method_addr;
	string   method_name;
        if (it.first.kernel_stack_id >= 0) {
            method_addr = *stacks.get_stack_addr(it.first.kernel_stack_id).begin();
            method_name = *stacks.get_stack_symbol(it.first.kernel_stack_id, -1).begin()+"[k]";
        }else if(it.first.user_stack_id >= 0) {
            method_addr = *stacks.get_stack_addr(it.first.user_stack_id).begin();
            method_name = *stacks.get_stack_symbol(it.first.user_stack_id, it.first.pid).begin();
        }
        struct method_type method = {.addr=method_addr, .ret=it.first.ret, .name=method_name};
	auto p = mout.find(method);
	if ( p==mout.end() ){
	    mout.insert(pair<method_type,int>(method, (int)it.second));
	}else{
	    (*p).second += it.second;    //merge_method from different callers
	}
	if( mout.size() >n ) break;
        fprintf(stdout,   "%ld\t %lx\t %lx\t, %lx\t %s\n", it.second, it.first.bp, it.first.ret, method_addr, method_name.c_str());
        //fprintf(out_cpu, "%ld\t %d\t  %d\t  %lx\t %s\n", it.second, it.first.user_stack_id, it.first.kernel_stack_id, method_addr, method_name.c_str());
    }
    fprintf(out_cpu, "samples\t method_addr\t method_name\n");

    multimap<int, method_type> rmap = flip_map(mout);
    //cout<<"flip method done, printing ("<< rmap.size()<<")"<<endl;
    int i=0;
    method_type methods[n];
    for (multimap<int,method_type>::const_reverse_iterator it = rmap.rbegin(); it!=rmap.rend(); ++it){
        fprintf(out_cpu, "%d\t %lx\t %s\n", it->first, it->second.addr, it->second.name.c_str() );
	if (i<n) methods[i++]=it->second;
    }
    //cout<<"method array done, printing ("<< n<<")"<<endl;
    if(COUNT_TOP_N>0){
        cout<<"start count monitoring..."<<endl;
        CountMethods(methods, COUNT_TOP_N);
    }
    if(LAT_TOP_N>0){
        cout<<"start latency measuring..."<<endl;
        for (int i=0; i<LAT_TOP_N; i++){
            LatencyMethod(methods[i]);
        }
    }
    fclose(out_cpu);
}
void PrintFlame(){
    auto table = bpf.get_hash_table<stack_key_t, uint64_t>("counts").get_table_offline();
    sort( table.begin(), table.end(),
      [](pair<stack_key_t, uint64_t> a, pair<stack_key_t, uint64_t> b) {
        return a.second < b.second;
      }
    );
    auto stacks = bpf.get_stack_table("stack_traces");
    stack<string> stack_traces;
    for (auto it : table) {
        //cout << "PID:" << it.first.pid << it.first.name << endl;
        if (it.first.kernel_stack_id >= 0) {
            auto syms = stacks.get_stack_symbol(it.first.kernel_stack_id, -1);
            for (auto sym : syms) {
                //fprintf(out_cpu, "%s;", sym.c_str());  //need to be reversed
                stack_traces.push(sym);
            }
        }
        if (it.first.user_stack_id >= 0) {
            auto syms = stacks.get_stack_symbol(it.first.user_stack_id, it.first.pid);
            for (auto sym : syms){
                //fprintf(out_cpu, "%s;", sym.c_str()); //need to be reversed
                stack_traces.push(sym);
            }
        }
        //fprintf(out_cpu, "%s;", string(it.first.name).c_str());
        fprintf(out_cpu, "%s;", it.first.name);
        while (!stack_traces.empty()){
            fprintf(out_cpu, "%s", stack_traces.top().c_str());
            stack_traces.pop();
	    if (!stack_traces.empty()){
	        fprintf(out_cpu, ";");
	    }
        }
        fprintf(out_cpu, "      %ld\n", it.second);
    }
    fclose(out_cpu);
}

void PrintBPF(int id){
    switch(id){
        case 0:
            PrintFlame();
	    break;
        case 1:
	    PrintTopMethods(SAMPLE_TOP_N);
	    break;
        case 2:
            PrintThread();
	    break;
    }
}
vector<string> parse_options(string str, char sep){
    istringstream ss(str);
    vector<string> kv;
    string sub;
    while (getline(ss,sub,sep)){
        kv.push_back(sub);
    }
    return kv;
}
string get_key(string kv, string sep){
    int i = kv.find(sep);
    return kv.substr(0,i);
}
string get_value(string kv, string sep){
    int i = kv.find(sep);
    return kv.substr(i+1);
}
bool str_contains(string s, string k){
    return s.find(k)==0;
}
///////////////////////////////////////////
void registerCapa(jvmtiEnv* jvmti){
    jvmtiCapabilities capa = {0};
    capa.can_tag_objects = 1;
    capa.can_generate_all_class_hook_events = 1;
    capa.can_generate_compiled_method_load_events = 1;
    //capa.can_generate_sampled_object_alloc_events = 1;
    //capa.can_generate_garbage_collection_events = 1;
    //capa.can_generate_object_free_events = 1;
    //capa.can_generate_vm_object_alloc_events = 1;
    jvmti->AddCapabilities(&capa);
}
void registerCall(jvmtiEnv* jvmti){
    jvmtiEventCallbacks call = {0};
    //call.SampledObjectAlloc = SampledObjectAlloc;
    //call.DataDumpRequest = DataDumpRequest;
    //call.VMDeath = VMDeath;
    //call.GarbageCollectionStart = GarbageCollectionStart;
    //call.GarbageCollectionFinish = GarbageCollectionFinish;
    call.CompiledMethodLoad = CompiledMethodLoad;
    call.CompiledMethodUnload = CompiledMethodUnload;
    call.DynamicCodeGenerated = DynamicCodeGenerated;
    jvmti->SetEventCallbacks(&call, sizeof(call));
}
void enableEvent(jvmtiEnv* jvmti){
    //jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_SAMPLED_OBJECT_ALLOC, NULL);
    //jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_DATA_DUMP_REQUEST, NULL);
    //jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_DEATH, NULL);
    //jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_GARBAGE_COLLECTION_START, NULL);
    //jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_GARBAGE_COLLECTION_FINISH, NULL);

    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_COMPILED_METHOD_LOAD, NULL);
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_COMPILED_METHOD_UNLOAD, NULL);
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_DYNAMIC_CODE_GENERATED, NULL);

    jvmti->GenerateEvents(JVMTI_EVENT_DYNAMIC_CODE_GENERATED);
    jvmti->GenerateEvents(JVMTI_EVENT_COMPILED_METHOD_LOAD);
}
void gen_perf_file(){
    registerCapa(jvmti);
    registerCall(jvmti);
    enableEvent(jvmti);
}
int do_single_options(string k, string v, jvmtiEnv* jvmti){
    if (k.compare("sample_duration") == 0){
        DURATION=stoi(v);
    }else if(k.compare("monitor_duration")==0){
        MON_DURATION=stoi(v);
    }else if(k.compare("frequency")==0){
        BPF_PERF_FREQ=stoi(v);
    }else if(k.compare("sample_top")==0){
        SAMPLE_TOP_N=stoi(v);
    }else if(k.compare("count_top")==0){
        COUNT_TOP_N=stoi(v);
    }else if(k.compare("lat_top")==0){
        LAT_TOP_N=stoi(v);
    }else if(k.compare("flame")==0){
        gen_perf_file();
        out_cpu = fopen(v.c_str(), "w");
        return 0;
    }else if(k.compare("sample_method")==0){
        gen_perf_file();
        out_cpu = fopen(v.c_str(), "w");
        return 1;
    }else if(k.compare("sample_thread")==0){
        out_thread = fopen(v.c_str(), "w");
        return 2;
    }
    //jvmti->SetHeapSamplingInterval(1024*1024); //1m
    return -1;
}
void InitFile() {
    //out_mem = stderr;
    out_cpu = stdout;
    out_thread = stdout;
    string pid = to_string(getpid());
    string path = "/tmp/perf-"+pid+".map";
    cout << "perf map: "<< path<< endl;
    out_perf = fopen(path.c_str(),"w");
}
JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* vm, char* options, void* reserved) {
    cout << "|***************************************|"<< endl;
    InitFile();
    genbpftextmap();
    vm->GetEnv((void**) &jvmti, JVMTI_VERSION_1_0);
    jvmti->CreateRawMonitor("tree_lock", &tree_lock);
    int id = -1;
    if (options != NULL) {
        vector<string> vkv = parse_options(string(options),';');
        for (string kv : vkv){
            string k = get_key(kv, "=");
            string v = get_value(kv, "=");
            cout << k << "=" << v << endl;
            int i = do_single_options(k,v,jvmti);
            if (i>-1) id=i;
        }
    }
    cout << "|***************************************|"<< endl;
    StartBPF(id);
    StopBPF();
    PrintBPF(id);
    //fclose(out_mem);
    cout << "Done."<< endl;
    return 0;
}

JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM* vm, char* options, void* reserved) {
    if (jvmti != NULL) {
        return 0;
    }
    return Agent_OnLoad(vm, options, reserved);
}
