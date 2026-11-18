#ifdef __3DS__
#include <3ds/3dslink.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
static bool remote_send(int fd, const void *data, size_t length)
{
    const char *bytes=data;
    u64 deadline=osGetTime()+15000;
    while (length) {
        u64 now=osGetTime();
        if (now>=deadline) return false;
        struct pollfd p={fd,POLLOUT,0};
        if (poll(&p,1,(int)(deadline-now))<=0 || !(p.revents&POLLOUT)) return false;
        ssize_t n=send(fd,bytes,length,0);
        if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) continue;
        if (n<=0) return false;
        bytes+=n; length-=(size_t)n;
    }
    return true;
}
static void remote_upload_checkpoint(const char *directory,const char *tag)
{
    if (!__3dslink_host.s_addr) return;
    void *buffer=memalign(0x1000,0x100000);
    if (!buffer) return;
    if (R_FAILED(socInit(buffer,0x100000))) { free(buffer); return; }
    int fd=link3dsConnectToHost(false,false);
    bool ok=fd>=0 && fcntl(fd,F_SETFL,O_NONBLOCK)==0;
    DIR *dir=ok?opendir(directory):NULL;
    if (!dir) ok=false;
    char greeting[96];
    int greet=snprintf(greeting,sizeof greeting,"FILMPROBE_FILES 2 %s\n",tag);
    if (ok) ok=greet>0 && greet<(int)sizeof greeting && remote_send(fd,greeting,(size_t)greet);
    struct dirent *entry;
    while (ok && (entry=readdir(dir))) {
        if (entry->d_name[0]=='.') continue;
        char path[512],header[512];
        snprintf(path,sizeof path,"%s/%s",directory,entry->d_name);
        FILE *file=fopen(path,"rb");
        if (!file) { ok=false; break; }
        if (fseek(file,0,SEEK_END)) { fclose(file); ok=false; break; }
        long length=ftell(file);
        if (length<0 || fseek(file,0,SEEK_SET)) { fclose(file); ok=false; break; }
        int n=snprintf(header,sizeof header,"FILE %s %ld\n",entry->d_name,length);
        ok=n>0 && n<(int)sizeof header && remote_send(fd,header,(size_t)n);
        char chunk[8192];
        while (ok && length>0) {
            size_t wanted=length>(long)sizeof chunk?sizeof chunk:(size_t)length;
            size_t got=fread(chunk,1,wanted,file);
            ok=got==wanted && remote_send(fd,chunk,got);
            length-=(long)got;
        }
        fclose(file);
    }
    if (dir) closedir(dir);
    if (ok) {
        ok=remote_send(fd,"DONE\n",5);
        struct pollfd p={fd,POLLIN,0}; char ack=0;
        ok=ok && poll(&p,1,15000)>0 && recv(fd,&ack,1,0)==1 && ack=='K';
    }
    printf("\nRemote evidence upload: %s\n",ok?"CONFIRMED":"FAILED; SD originals retained");
    if (fd>=0) close(fd);
    socExit(); free(buffer);
}
#else
static void remote_upload_checkpoint(const char *directory,const char *tag) { (void)directory;(void)tag; }
#endif
