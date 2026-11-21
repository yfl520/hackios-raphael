/* Read-only geometry from the loader's existing immutable partition table.
 * len=0 only obtains metadata: no UFS transfer, scan, or write is performed.
 */
static int
ufs_sysctl_darwin_capacity(__unused struct sysctl_oid *oidp,
    __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
    uint64_t bytes=0;
    int error=hakios_ufs_read_partition("darwin",0,NULL,0,&bytes);
    if(error)return error;
    if(bytes==0 || bytes>0x7fffffffffffffffULL)return EINVAL;
    return sysctl_io_number(req,(long long)bytes,sizeof(bytes),NULL,NULL);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,darwin_capacity,
    CTLTYPE_QUAD|CTLFLAG_RD|CTLFLAG_LOCKED,NULL,0,ufs_sysctl_darwin_capacity,
    "Q","actual darwin partition capacity in bytes; excludes Android storage");
