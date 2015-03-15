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

- (void)applicationDidFinishLaunching:(NSNotification *)aNotification {
    NSLog(@"hereComes!");
    
//    https://github.com/sassembla/sctp-refimpl　からひっぱってきて、使い方を見よう。Cで書く事に別に悪いところは無いわ。
}

- (void)applicationWillTerminate:(NSNotification *)aNotification {
    // Insert code here to tear down your application
}

@end
