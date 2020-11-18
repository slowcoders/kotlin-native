//
//  main.m
//  kobjc
//
//  Created by DaeHoon Zee on 2020/06/01.
//  Copyright © 2020 DaeHoon Zee. All rights reserved.
//

#import <Cocoa/Cocoa.h>
#import <Test2/Test2.h>

extern void cpu_cache_test();
extern void lzay_delloc_test();

@interface SslSocket0 : NSObject

@end

@implementation SslSocket0
    
+ (void)initialize {
    NSLog(@"Socket 0 initialized");
}
@end

@interface SslSocket1 : SslSocket0

@end

@implementation SslSocket1
    
+ (void)initialize {
    NSLog(@"Socket 1 initialized");
}
@end

@interface SslSocket2 : SslSocket0

@end

@implementation SslSocket2
    
+ (void)initialize {
    NSLog(@"Socket 2 initialized");
}
@end

@interface SslSocket3 : SslSocket0

@end

@implementation SslSocket3
    
@end


int main(int argc, char * argv[])
{
    Class c1 = SslSocket1.class;
    Class c2 = SslSocket2.class;
    Class c3 = SslSocket3.class;


    @autoreleasepool {
//        int* cc = NULL;
//        int i0 = cc[0];
//        int i1 = cc[1];
//        int i2 = cc[2];
//        assert(cc != NULL);

        
        lzay_delloc_test();
        
        [Test2HelloKt forIntegersB:1 s:1 i:3 l:[Test2ULong numberWithUnsignedLongLong:4]];
//        [Test2HelloKt forIntegersB:1 s:1 i:3 l:nil];
//        
//        [Test2HelloKt forFloatsIfc:nil d:[Test2Double numberWithDouble:2.71]];
//        [Test2HelloKt forFloatsIfc:nil d:[Test2Double numberWithDouble:2.71]];
//        
//        NSString* ret = @"TTTT";
//        [Test2HelloKt acceptFunF:^NSString * _Nullable(NSString * it) {
//            return [it stringByAppendingString:@" Kotlin is fun"];
//        }];
//        
//        NSLog(@"%@", ret);
        return 0;
        
    }
    return NSApplicationMain(argc, argv);
}
