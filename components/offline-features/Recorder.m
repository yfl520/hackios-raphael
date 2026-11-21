#import <UIKit/UIKit.h>
#import <AVKit/AVKit.h>
#import <objc/message.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#define RECORD_DIR @"/var/mobile/Media/HakiOSRecordings"
static NSDictionary *RecorderCommand(NSString *command) {
 int fd=socket(AF_UNIX,SOCK_STREAM,0);
 if (fd<0) return @{@"error":@"无法创建本机连接"};
 struct timeval tv={2,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
 struct sockaddr_un addr={0};addr.sun_family=AF_UNIX;strlcpy(addr.sun_path,"/var/run/raphael-recorder.sock",sizeof(addr.sun_path));
 if (connect(fd,(struct sockaddr *)&addr,sizeof(addr))) {close(fd);return @{@"error":@"录屏服务未就绪，请稍后再试"};}
 NSData *data=[[command stringByAppendingString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding];
 const uint8_t *p=data.bytes;size_t sent=0;
 while (sent<data.length) {ssize_t n=send(fd,p+sent,data.length-sent,0);if(n<=0) break;sent+=(size_t)n;}
 NSMutableData *response=[NSMutableData data];uint8_t b[1024];
 for (;;) {ssize_t n=recv(fd,b,sizeof(b),0);if(n<=0)break;[response appendBytes:b length:(NSUInteger)n];if(response.length>8192)break;}
 close(fd);
 id value=[NSJSONSerialization JSONObjectWithData:response options:0 error:nil];
 return [value isKindOfClass:[NSDictionary class]]?value:@{@"error":@"无法读取服务响应"};
}
@interface RecorderController : UITableViewController
@property UILabel *statusLabel;
@property UIButton *startButton;
@property UIButton *stopButton;
@property UISegmentedControl *quality;
@property NSArray<NSString *> *files;
@property NSTimer *timer;
@property BOOL querying;
@end
@implementation RecorderController
- (void)viewDidLoad {
 [super viewDidLoad];self.title=@"屏幕录制";self.view.backgroundColor=UIColor.systemGroupedBackgroundColor;
 self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh target:self action:@selector(refreshFiles)];
 UIStackView *stack=[[UIStackView alloc] init];stack.axis=UILayoutConstraintAxisVertical;stack.spacing=14;stack.layoutMargins=UIEdgeInsetsMake(20,20,20,20);stack.layoutMarginsRelativeArrangement=YES;
 UILabel *intro=[[UILabel alloc] init];intro.numberOfLines=0;intro.font=[UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
 intro.text=@"点击开始，3 秒后录制整个屏幕。返回此应用点击停止，文件会保存在本机。此实验版使用软件 JPEG 编码，无音频；相机、Wi-Fi 等沿用 R13。";
 [stack addArrangedSubview:intro];
 self.quality=[[UISegmentedControl alloc] initWithItems:@[@"360p · 10帧",@"540p · 10帧",@"360p · 15帧"]];self.quality.selectedSegmentIndex=0;[stack addArrangedSubview:self.quality];
 self.statusLabel=[[UILabel alloc] init];self.statusLabel.numberOfLines=0;self.statusLabel.text=@"正在连接录屏服务…";[stack addArrangedSubview:self.statusLabel];
 self.startButton=[UIButton buttonWithType:UIButtonTypeSystem];self.startButton.configuration=[UIButtonConfiguration filledButtonConfiguration];[self.startButton setTitle:@"开始录制" forState:UIControlStateNormal];[self.startButton addTarget:self action:@selector(start) forControlEvents:UIControlEventTouchUpInside];[stack addArrangedSubview:self.startButton];
 self.stopButton=[UIButton buttonWithType:UIButtonTypeSystem];self.stopButton.configuration=[UIButtonConfiguration borderedButtonConfiguration];[self.stopButton setTitle:@"停止并保存" forState:UIControlStateNormal];[self.stopButton addTarget:self action:@selector(stop) forControlEvents:UIControlEventTouchUpInside];[stack addArrangedSubview:self.stopButton];
 UILabel *limit=[[UILabel alloc] init];limit.numberOfLines=0;limit.text=@"最多 10 分钟或 512 MiB，剩余空间不足时自动停止。点下面文件预览；长按可分享、保存至文件或相册。横屏内容按手机竖向画布保存。";limit.font=[UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];limit.textColor=UIColor.secondaryLabelColor;[stack addArrangedSubview:limit];
 stack.frame=CGRectMake(0,0,self.view.bounds.size.width,440);self.tableView.tableHeaderView=stack;
 UILongPressGestureRecognizer *longPress=[[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(share:)];[self.tableView addGestureRecognizer:longPress];
 [self refreshFiles];
 self.timer=[NSTimer scheduledTimerWithTimeInterval:1 target:self selector:@selector(updateStatus) userInfo:nil repeats:YES];[self updateStatus];
 [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(becameActive) name:UIApplicationDidBecomeActiveNotification object:nil];
}
- (void)viewDidLayoutSubviews {
 [super viewDidLayoutSubviews];UIView *header=self.tableView.tableHeaderView;
 CGSize size=[header systemLayoutSizeFittingSize:CGSizeMake(self.tableView.bounds.size.width,0) withHorizontalFittingPriority:UILayoutPriorityRequired verticalFittingPriority:UILayoutPriorityFittingSizeLevel];
 if (fabs(header.frame.size.height-size.height)>1) {header.frame=CGRectMake(0,0,self.tableView.bounds.size.width,size.height);self.tableView.tableHeaderView=header;}
}
- (void)becameActive { [self refreshFiles];[self updateStatus]; }
- (void)applyStatus:(NSDictionary *)d {
 BOOL active=[d[@"active"] boolValue];NSString *error=d[@"error"];
 self.startButton.enabled=!active;self.stopButton.enabled=active;self.quality.enabled=!active;
 if (error.length) self.statusLabel.text=[NSString stringWithFormat:@"状态：%@",error];
 else if(active) self.statusLabel.text=[NSString stringWithFormat:@"🔴 录制中 · %.0f 秒 · 已保存 %@ 帧\n返回本应用即可停止。",[d[@"elapsed"] doubleValue],d[@"frames"]?:@0];
 else self.statusLabel.text=@"就绪 · 录制由你手动开始";
}
- (void)updateStatus {
 if (self.querying) return;self.querying=YES;
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{NSDictionary *d=RecorderCommand(@"STATUS");dispatch_async(dispatch_get_main_queue(),^{self.querying=NO;[self applyStatus:d];});});
}
- (void)start {
 self.startButton.enabled=NO;
 NSInteger choice=self.quality.selectedSegmentIndex;int step=choice==1?2:3,fps=choice==2?15:10;
 NSString *command=[NSString stringWithFormat:@"START %d %d",step,fps];
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{NSDictionary *d=RecorderCommand(command);dispatch_async(dispatch_get_main_queue(),^{
  [self applyStatus:d];if (![d[@"active"] boolValue]) return;
  self.statusLabel.text=@"3 秒后开始录制，即将返回桌面…";
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(1*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
   UIApplication *app=UIApplication.sharedApplication;SEL s=NSSelectorFromString(@"suspend");
   if([app respondsToSelector:s]) ((void(*)(id,SEL))objc_msgSend)(app,s);
  });
 });});
}
- (void)stop {
 self.stopButton.enabled=NO;
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{NSDictionary *d=RecorderCommand(@"STOP");dispatch_async(dispatch_get_main_queue(),^{[self applyStatus:d];[self refreshFiles];});});
}
- (void)refreshFiles {
 NSArray *names=[[NSFileManager defaultManager] contentsOfDirectoryAtPath:RECORD_DIR error:nil];
 self.files=[[names filteredArrayUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(NSString *name,NSDictionary *bindings){(void)bindings;return [name.pathExtension.lowercaseString isEqualToString:@"mov"];}]] sortedArrayUsingComparator:^NSComparisonResult(NSString *a,NSString *b){return [b compare:a];}];
 [self.tableView reloadData];
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section { (void)tableView;(void)section;return self.files.count; }
- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {(void)tableView;(void)section;return self.files.count?@"本机录制 · 点击预览，长按分享":@"尚无录制文件";}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
 UITableViewCell *cell=[tableView dequeueReusableCellWithIdentifier:@"file"];
 if(!cell)cell=[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"file"];
 NSString *name=self.files[indexPath.row];cell.textLabel.text=name;
 NSDictionary *a=[[NSFileManager defaultManager] attributesOfItemAtPath:[RECORD_DIR stringByAppendingPathComponent:name] error:nil];
 cell.detailTextLabel.text=[NSByteCountFormatter stringFromByteCount:[a[NSFileSize] longLongValue] countStyle:NSByteCountFormatterCountStyleFile];cell.accessoryType=UITableViewCellAccessoryDisclosureIndicator;return cell;
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
 [tableView deselectRowAtIndexPath:indexPath animated:YES];NSURL *url=[NSURL fileURLWithPath:[RECORD_DIR stringByAppendingPathComponent:self.files[indexPath.row]]];
 AVPlayerViewController *vc=[[AVPlayerViewController alloc] init];vc.player=[AVPlayer playerWithURL:url];[self presentViewController:vc animated:YES completion:^{[vc.player play];}];
}
- (void)share:(UILongPressGestureRecognizer *)gesture {
 if(gesture.state!=UIGestureRecognizerStateBegan)return;
 NSIndexPath *index=[self.tableView indexPathForRowAtPoint:[gesture locationInView:self.tableView]];if(!index || index.row>=self.files.count)return;
 NSURL *url=[NSURL fileURLWithPath:[RECORD_DIR stringByAppendingPathComponent:self.files[index.row]]];
 UIActivityViewController *vc=[[UIActivityViewController alloc] initWithActivityItems:@[url] applicationActivities:nil];
 vc.popoverPresentationController.sourceView=[self.tableView cellForRowAtIndexPath:index];[self presentViewController:vc animated:YES completion:nil];
}
@end
@interface RecorderApp : UIResponder<UIApplicationDelegate>
@property UIWindow *window;
@end
@implementation RecorderApp
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
 (void)application;(void)options;self.window=[[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
 self.window.rootViewController=[[UINavigationController alloc] initWithRootViewController:[[RecorderController alloc] initWithStyle:UITableViewStyleInsetGrouped]];
 [self.window makeKeyAndVisible];return YES;
}
@end
int main(int argc,char **argv) {@autoreleasepool{return UIApplicationMain(argc,argv,nil,NSStringFromClass(RecorderApp.class));}}
