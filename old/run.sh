SRC=profiler.cpp
AGT=prof
AGENT=lib$AGT.so
BIN=jbprof
sudo rm -rf $AGENT $BIN log thread.log cpu.log mem.log hs_err* jcmd.log /tmp/perf* /tmp/.java_pid* .attach_pid*  #flame.svg
sudo kill -9 `pgrep java`
LOOP="2000 4000000"
JIT="-Xmx400m -Xms10m -XX:+UseParallelGC -XX:ParallelGCThreads=1 -XX:+PreserveFramePointer" # -XX:+DTraceMethodProbes" #-XX:+ExtendedDTraceProbes

JAVA_HOME=/home/sun/jbb/jdk
java_build(){
    $JAVA_HOME/bin/javac Main.java
}
cpp_build(){
  BCC=/home/sun/jbb/bcc
  BCC_INC="-I$BCC/src/cc -I$BCC/src/cc/api -I$BCC/src/cc/libbpf/include/uapi"
  CC=clang
  CPP=g++
  #CPP=clang++
  OS=linux
  JAVA_INC="-I$JAVA_HOME/include -I$JAVA_HOME/include/$OS"
  #export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib/
  #export LIBRARY_PATH=$LIBRARY_PATH:/usr/local/lib/
  LIB="-lbcc"
  #LIB="-shared -lbcc -lstdc++ "
  OPTS="-O3 -fPIC -shared $JAVA_INC $BCC_INC $LIB"
  echo "$CPP $SRC $OPTS -o $AGENT"
        $CPP $SRC $OPTS -o $AGENT
  echo "agent done."
  OPTS="-g -O0 $JAVA_INC $BCC_INC $LIB"
  OPTS="-O3 $JAVA_INC $BCC_INC $LIB"
  echo "$CPP $SRC $OPTS -o $BIN"
        $CPP $SRC $OPTS -o $BIN
  #echo "binary done."
}

attach(){
    echo "$JAVA_HOME/bin/java $JIT Main $LOOP"
    time $JAVA_HOME/bin/java $JIT Main $LOOP &
    sleep 1
    pid=`pgrep java`
    OPT=$1
    #export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:`pwd`
    echo "./$BIN $pid `pwd`/$AGENT $OPT"
    sudo ./$BIN $pid `pwd`/$AGENT "$OPT"
}
run_and_attach(){
    AGT=$1
    OPT=$2
    time $JAVA_HOME/bin/java $JIT Main $LOOP &
    sleep 1
    pid=`pgrep java`
    #/usr/share/bcc/tools/tplist -p $pid
    echo "$JAVA_HOME/bin/jcmd $pid JVMTI.agent_load ./$AGT $OPT"
    sudo  $JAVA_HOME/bin/jcmd $pid JVMTI.agent_load ./$AGT "\"$OPT\"" > jcmd.log 2>&1
    #python method.py -F 99 -p $pid -f 3 > profile.out
}

run_with_agent(){
    AGT=$1
    OPT=$2
    #-XX:+EnableJVMCI -XX:+UseJVMCICompiler -XX:-TieredCompilation -XX:+PrintCompilation -XX:+UnlockExperimentalVMOptions 
    echo "$JAVA_HOME/bin/java $JIT -agentpath:./$AGT=$OPT Main $LOOP"
    time $JAVA_HOME/bin/java $JIT -agentpath:./$AGT=$OPT Main $LOOP 
}
#java_build
cpp_build
if [ $? = 0 ]; then
    cp $AGENT lib/
    #run_and_attach $AGENT "sample_duration=5;frequency=49;flame=cpu.log"
    #./flamegraph.pl cpu.log > flame.svg
    ###########run_with_agent $AGENT "sample_duration=5;sample_mem=mem.log"

    #run_and_attach $AGENT "sample_duration=5;frequency=49;sample_thread=4;log_file=thread.log"

    #run_and_attach $AGENT "sample_duration=3;sample_method=9;log_file=method.log"

    #run_and_attach $AGENT "sample_duration=3;sample_method=9;log_file=method.log;monitor_duration=1;count_top=1"
    #run_and_attach $AGENT "sample_duration=5;sample_method=9;log_file=method.log;monitor_duration=1;lat_top=2"

    #run_with_agent $AGENT "sample_duration=10;sample_mem=9;log_file=mem.log;count_alloc=1"
    #run_with_agent $AGENT "sample_duration=10;sample_mem=9;log_file=mem.log;mon_size=1"

    #run_and_attach $AGENT "sample_duration=3;sample_method=9;log_file=method.log;rule_cfg=tune.cfg;wait=1"
    #run_and_attach $AGENT "sample_duration=3;sample_method=9;log_file=method.log;rule_cfg=tune.cfg;action_n=3;start_until=.PROF%start"

    attach "sample_duration=3;sample_method=9;log_file=method.log;rule_cfg=tune.cfg;action_n=3;start_until=.PROF%start"





    #run_with_agent $AGENT "sample_duration=5;sample_top=9;sample_method=method.log;tune_fields=tune.cfg"
    #echo "rule : when HashMap.resize  -> + initial_capacity"
    #echo "rule :      HashMap.getNode -> - loadFactor "

    #HashMap.DEFAULT_INITIAL_CAPACITY name#363 length=2, value#364 value=0x00000010 (16) flag=0x0018 (static final), type=I (int)
    #00000010 -> 16
    #00000040 -> 64
    #00000100 -> 256
    #00000400 -> 1024
    #HashMap.DEFAULT_LOAD_FACTOR name#366, length=2, value#92, value=0x3f400000 (0.75), flag=0x0018 (static final), type=F (float)
    #3f400000 -> 0.75
    #3f000000 -> 0.5
    #3e800000 -> 0.25
    #/usr/share/bcc/tools/funclatency -d 3 c:malloc
fi
