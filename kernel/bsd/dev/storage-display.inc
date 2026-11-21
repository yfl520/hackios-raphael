/* Preferences-only adaptation of the native StorageData monitor. */
@interface NSObject (RaphaelStorageSpace)
- (id)initWithTotal:(long long)total free:(long long)free purgeable:(long long)purgeable;
@end
static void (*storage_old_sync)(id,SEL);
static long long (*storage_old_device_size)(id,SEL);
static Ivar storage_space_ivar;
static Class storage_space_class;
static BOOL storage_installed;
static unsigned storage_report_count;

static BOOL storage_current(struct raphael_storage_counts *out) {
    uint64_t partition=0;size_t length=sizeof(partition);
    struct statfs data={0};
    if(sysctlbyname("kern.hakios.darwin_capacity",&partition,&length,NULL,0) ||
        length!=sizeof(partition) || statfs("/private/var",&data) ||
        strcmp(data.f_fstypename,"hfs") || (data.f_flags&MNT_RDONLY) ||
        strcmp(data.f_mntfromname,"/dev/ufs_darwin_data"))return NO;
    return raphael_storage_account(partition,data.f_bsize,data.f_blocks,
        data.f_bfree,data.f_bavail,out);
}
static void storage_sync(id self,SEL sel) {
    struct raphael_storage_counts counts;
    if(!storage_current(&counts)){
        storage_old_sync(self,sel);return;
    }
    id space=[[storage_space_class alloc] initWithTotal:(long long)counts.total
        free:(long long)counts.available purgeable:0];
    if(!space){storage_old_sync(self,sel);return;}
    object_setIvarWithStrongDefault(self,storage_space_ivar,space);
    if(__sync_fetch_and_add(&storage_report_count,1)<4)
        note("storage-display: native monitor total=%llu available=%llu used=%llu reserved=%llu data-used=%llu\n",
            (unsigned long long)counts.total,(unsigned long long)counts.available,
            (unsigned long long)counts.used,(unsigned long long)counts.reserved,
            (unsigned long long)counts.data_used);
}
static long long storage_device_size(id self,SEL sel) {
    struct raphael_storage_counts counts;
    return storage_current(&counts)?(long long)counts.total:storage_old_device_size(self,sel);
}
static BOOL storage_abi(Method method,const char *result,unsigned count,const char *const *arguments) {
    if(!method || method_getNumberOfArguments(method)!=count+2)return NO;
    char *type=method_copyReturnType(method);
    BOOL valid=type&&!strcmp(type,result);free(type);
    for(unsigned i=0;valid&&i<count;i++){
        type=method_copyArgumentType(method,i+2);
        valid=type&&!strcmp(type,arguments[i]);free(type);
    }
    return valid;
}
static void storage_install(void) {
    if(storage_installed || ![NSProcessInfo.processInfo.processName isEqual:@"Preferences"])return;
    Class cls=NSClassFromString(@"STStorageDiskMonitor");
    Class space=NSClassFromString(@"STStorageSpace");
    if(!cls||!space)return;
    Method sync=class_getInstanceMethod(cls,sel_registerName("sync"));
    Method size=class_getInstanceMethod(cls,sel_registerName("deviceSize"));
    Method ctor=class_getInstanceMethod(space,sel_registerName("initWithTotal:free:purgeable:"));
    const char *args[]={"q","q","q"};
    Ivar ivar=class_getInstanceVariable(cls,"_storageSpace");
    if(!storage_abi(sync,"v",0,NULL)||!storage_abi(size,"q",0,NULL)||
        !storage_abi(ctor,"@",3,args)||!ivar||ivar_getTypeEncoding(ivar)[0]!='@'){
        note("storage-display: native ABI mismatch; original monitor retained\n");return;
    }
    storage_space_class=space;storage_space_ivar=ivar;
    storage_old_sync=(void(*)(id,SEL))method_getImplementation(sync);
    storage_old_device_size=(long long(*)(id,SEL))method_getImplementation(size);
    method_setImplementation(sync,(IMP)storage_sync);
    method_setImplementation(size,(IMP)storage_device_size);
    storage_installed=YES;
    note("storage-display: native monitor adapted to darwin partition and persistent HFS; no capacity API interception\n");
}
