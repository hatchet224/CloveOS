#ifdef NDEBUG
#error "Input migration tests require active assertions"
#endif
#include "../src/qualification.c"
#include <assert.h>
int main(void)
{
 struct cl_input_verification v={0},decoded,sentinel;char bytes[CL_INPUT_RECORD_MAX],canonical[CL_INPUT_RECORD_MAX],digest[65];size_t count,used;
 strcpy(v.baseline.guid,"1900385212b400006666000000010000");v.baseline.hat=-1;
 for(size_t i=0;i<CL_CONTROL_COUNT;++i)v.baseline.button[i]=(int)i;
 v.baseline.button[CL_START]=22;v.baseline.button[CL_SELECT]=23;v.checked=v.baseline;
 assert(cl_input_record_encode("C028-0002",&v,bytes,&count));assert(strstr(bytes,"\"schema\":2")&&strstr(bytes,"\"start\":22")&&strstr(bytes,"\"select\":23"));
 assert(cl_input_record_decode(bytes,count,"C028-0002",&v.baseline,&decoded));assert(decoded.checked.button[CL_START]==22);
 int swap=v.checked.button[CL_A];v.checked.button[CL_A]=v.checked.button[CL_B];v.checked.button[CL_B]=swap;
 swap=v.checked.button[CL_X];v.checked.button[CL_X]=v.checked.button[CL_Y];v.checked.button[CL_Y]=swap;
 assert(body("C028-0002",&v,1U,canonical,&used));cs_sha256_hex(canonical,used,digest);
 int n=snprintf(bytes,sizeof(bytes),"%.*s,\"checksum\":\"%s\"}\n",(int)(used-1U),canonical,digest);assert(n>0);
 assert(cl_input_record_decode(bytes,(size_t)n,"C028-0002",&v.baseline,&decoded));
 assert(decoded.baseline.button[CL_START]==22&&decoded.checked.button[CL_SELECT]==23);
 assert(decoded.checked.button[CL_A]==v.baseline.button[CL_B]&&decoded.checked.button[CL_X]==v.baseline.button[CL_Y]);
 sentinel=decoded;bytes[30]^=1;assert(!cl_input_record_decode(bytes,(size_t)n,"C028-0002",&v.baseline,&decoded));assert(memcmp(&decoded,&sentinel,sizeof(decoded))==0);
 v.checked.button[CL_START]=24;assert(!cl_input_verification_valid(&v.baseline,&v));v.checked=v.baseline;
 assert(mkdir("/run/console",0700)==0||errno==EEXIST);assert(mkdir("/run/console/config",0700)==0||errno==EEXIST);
 assert(cl_input_runtime_publish(&v));int fd=open("/run/console/config/verified-input.json",O_RDONLY|O_NOFOLLOW);assert(fd>=0);ssize_t size=read(fd,bytes,sizeof(bytes)-1U);assert(size>0);bytes[(size_t)size]='\0';assert(close(fd)==0);
 assert(strstr(bytes,"\"schema\":2")&&strstr(bytes,"\"start\":22")&&strstr(bytes,"\"select\":23"));
 puts("PASS schema2 measured START/SELECT, schema1 checksum migration, forged swap rejection, protected runtime publish");return 0;
}
