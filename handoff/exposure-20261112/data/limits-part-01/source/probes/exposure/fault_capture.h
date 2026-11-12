#pragma once
#ifdef __3DS__
/* Fatal diagnostics only: never recover execution from an exception. */
static u8 probe_exception_stack[16384] __attribute__((aligned(8)));
static ERRF_ExceptionData probe_exception_data;
static char probe_fault_path[240]="sdmc:/3ds/Film3DS/probe-fault.dmp";
static char probe_lifecycle_path[240]="sdmc:/3ds/Film3DS/probe-lifecycle.txt";
static aptHookCookie probe_apt_cookie;
static void probe_apt_event(APT_HookType type,void *unused)
{
    (void)unused;
    const char *names[]={"suspend","restore","sleep","wakeup","exit"};
    const char *name=(unsigned)type<5?names[type]:"unknown";
    FILE *file=fopen(probe_lifecycle_path,"a");
    if(file){fprintf(file,"APT type=%u name=%s time_ms=%llu\n",(unsigned)type,name,(unsigned long long)osGetTime());fclose(file);}
    printf("\nAPT event: %s\n",name);
}
static void __attribute__((target("arm"),noreturn)) probe_exception(ERRF_ExceptionInfo *info,CpuRegisters *regs)
{
    ERRF_ExceptionData saved;memcpy(&saved.excep,info,sizeof *info);memcpy(&saved.regs,regs,sizeof *regs);
    FILE *file=fopen(probe_fault_path,"wb");
    if(file){fwrite(&saved,1,sizeof saved,file);fclose(file);}
    printf("\nPROBE FAULT type=%u PC=%08lx FAR=%08lx\nFSR=%08lx FPEXC=%08lx\n%s\nRestart after recording this screen.\n",
        (unsigned)info->type,(unsigned long)regs->pc,(unsigned long)info->far,
        (unsigned long)info->fsr,(unsigned long)info->fpexc,probe_fault_path);
    /* Keep fault evidence visible; no attempt to reuse a potentially live DMA. */
    for(;;)svcSleepThread(INT64_C(1000000000));
}
static void probe_fault_install(void)
{
    threadOnException(probe_exception,probe_exception_stack+sizeof probe_exception_stack,&probe_exception_data);
    aptHook(&probe_apt_cookie,probe_apt_event,NULL);
}
#endif
