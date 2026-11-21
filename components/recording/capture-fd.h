/* Single-descriptor local handoff. No path, image data, or remote access. */
#ifndef RAPHAEL_CAPTURE_FD_H
#define RAPHAEL_CAPTURE_FD_H
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
static inline int capture_send_fd(int socket_fd,int device_fd,int error) {
 struct iovec io={&error,sizeof(error)};
 union {struct cmsghdr alignment;char bytes[CMSG_SPACE(sizeof(int))];} ancillary;
 memset(&ancillary,0,sizeof(ancillary));
 struct msghdr message={0};message.msg_iov=&io;message.msg_iovlen=1;
 if(device_fd>=0) {
  message.msg_control=ancillary.bytes;message.msg_controllen=sizeof(ancillary.bytes);
  struct cmsghdr *header=CMSG_FIRSTHDR(&message);header->cmsg_level=SOL_SOCKET;
  header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(header),&device_fd,sizeof(int));
 }
 return sendmsg(socket_fd,&message,0)==sizeof(error)?0:-1;
}
static inline int capture_receive_fd(int socket_fd) {
 int error=EPROTO,result=-1;struct iovec io={&error,sizeof(error)};
 union {struct cmsghdr alignment;char bytes[CMSG_SPACE(sizeof(int)*8)];} ancillary;
 memset(&ancillary,0,sizeof(ancillary));
 struct msghdr message={0};message.msg_iov=&io;message.msg_iovlen=1;
 message.msg_control=ancillary.bytes;message.msg_controllen=sizeof(ancillary.bytes);
 ssize_t count=recvmsg(socket_fd,&message,MSG_WAITALL);
 for(struct cmsghdr *header=CMSG_FIRSTHDR(&message);header;header=CMSG_NXTHDR(&message,header)) {
  if(header->cmsg_level!=SOL_SOCKET || header->cmsg_type!=SCM_RIGHTS || header->cmsg_len<CMSG_LEN(sizeof(int)))continue;
  size_t descriptors=(header->cmsg_len-CMSG_LEN(0))/sizeof(int);
  for(size_t index=0;index<descriptors;index++) {
   int fd;memcpy(&fd,(char *)CMSG_DATA(header)+index*sizeof(int),sizeof(fd));
   if(result<0)result=fd;else {close(fd);error=EPROTO;}
  }
 }
 if(count!=sizeof(error) || message.msg_flags&(MSG_CTRUNC|MSG_TRUNC) || error || result<0) {
  if(result>=0)close(result);errno=error?error:EPROTO;return -1;
 }
 return result;
}
#endif
