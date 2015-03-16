//
//  AppDelegate.m
//  SCTPs
//
//  Created by illusionismine on 2015/03/16.
//  Copyright (c) 2015年 KISSAKI. All rights reserved.
//

#import "AppDelegate.h"
#import "usrsctp.h"

@interface AppDelegate ()

@property (weak) IBOutlet NSWindow *window;
@end

@implementation AppDelegate



int done = 0;

static int
receive_cb(struct socket *sock, union sctp_sockstore addr, void *data,
           size_t datalen, struct sctp_rcvinfo rcv, int flags, void *ulp_info)
{
    if (data == NULL) {
        done = 1;
        usrsctp_close(sock);
    } else {
        write(fileno(stdout), data, datalen);
        free(data);
    }
    return 1;
}

- (void)applicationDidFinishLaunching:(NSNotification *)aNotification {
    
    char *argv_1 = "hddsaa";
    char *argv_2 = "hddsa";
    char *argv_3 = "hjk";
    char *argv_4 = "h";
    int argc = 0;
    
    NSLog(@"hereComes!");
    
    struct socket *sock;
    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;
    struct sctp_udpencaps encaps;

    char* buffer[80];
    
    if (argc > 3) {
        usrsctp_init(atoi(argv_3));
    } else {
        usrsctp_init(9899);
    }
    
//    usrsctp_sysctl_set_sctp_debug_on(0);
    usrsctp_sysctl_set_sctp_blackhole(2);
    if ((sock = usrsctp_socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP, receive_cb, NULL, 0, NULL)) == NULL) {
        perror("userspace_socket ipv6");
    }
    
    if (argc > 4) {
        memset(&encaps, 0, sizeof(struct sctp_udpencaps));
        encaps.sue_address.ss_family = AF_INET6;
        encaps.sue_port = htons(atoi(argv_4));
        if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_REMOTE_UDP_ENCAPS_PORT, (const void*)&encaps, (socklen_t)sizeof(struct sctp_udpencaps)) < 0) {
            perror("setsockopt");
        }
    }

    
    memset((void *)&addr4, 0, sizeof(struct sockaddr_in));
    memset((void *)&addr6, 0, sizeof(struct sockaddr_in6));
#if !defined(__Userspace_os_Linux) && !defined(__Userspace_os_Windows)
    addr4.sin_len = sizeof(struct sockaddr_in);
    addr6.sin6_len = sizeof(struct sockaddr_in6);
#endif
    addr4.sin_family = AF_INET;
    addr6.sin6_family = AF_INET6;
    addr4.sin_port = htons(atoi(argv_2));
    addr6.sin6_port = htons(atoi(argv_2));
    if (inet_pton(AF_INET6, argv_1, &addr6.sin6_addr) == 1) {
        if (usrsctp_connect(sock, (struct sockaddr *)&addr6, sizeof(struct sockaddr_in6)) < 0) {
            perror("userspace_connect");
        }
    } else if (inet_pton(AF_INET, argv_1, &addr4.sin_addr) == 1) {
        if (usrsctp_connect(sock, (struct sockaddr *)&addr4, sizeof(struct sockaddr_in)) < 0) {
            perror("userspace_connect");
        }
    } else {
        printf("Illegal destination address.\n");
    }
    while ((fgets(buffer, sizeof(buffer), stdin) != NULL) && !done) {
        usrsctp_sendv(sock, buffer, strlen(buffer), NULL, 0,
				                  NULL, 0, SCTP_SENDV_NOINFO, 0);
    }
    if (!done) {
        usrsctp_shutdown(sock, SHUT_WR);
    }
    while (!done) {
#if defined (__Userspace_os_Windows)
        Sleep(1*1000);
#else
        sleep(1);
#endif
    }
    while (usrsctp_finish() != 0) {
#if defined (__Userspace_os_Windows)
        Sleep(1000);
#else
        sleep(1);
#endif
    }
//    https://github.com/sassembla/sctp-refimpl　からひっぱってきて、使い方を見よう。Cで書く事に別に悪いところは無いわ。
}




- (void)applicationWillTerminate:(NSNotification *)aNotification {
    // Insert code here to tear down your application
}

@end
