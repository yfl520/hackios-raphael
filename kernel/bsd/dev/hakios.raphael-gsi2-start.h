/* Secure PIL startup: read EE0 readiness only, never write GSI globals. */
static bool ripa_gsi_ready(void)
{
    ((void)0);
    need(&ripa_gsi);
    if(!ripa_gsi.base)return false;
    uint32_t status=rd(ripa_gsi.base);
    ((void)0);
    return (status&1)!=0;
}
