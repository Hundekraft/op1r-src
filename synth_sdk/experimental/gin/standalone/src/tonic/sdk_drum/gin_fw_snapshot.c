/* Copyright (c) 2026 SquareWave Studio. SDK-derived portions (c) 2026 pcppcp.
 * GPL-3.0-or-later with OP1R additional permission; see LICENSE and NOTICE. */


#include "gin_fw.h"
#include "gin_fw_snapshot.h"
#include "gin_snapshot_text.h"
#include <stddef.h>
/* 5000 bytes fit the patch Gin builds in a session (4,047 serialized), but not the one R246 reloads after a reboot:
 * its normalize step adds "dbox_data", 24 x 8 values (null there, 1,200 bytes), so the serialized patch is 5,040 bytes
 * and every working save after a reboot failed (OP1_DRUM_SNAPSHOT). Upper bound of a patch: the state 3,760 + dbox_data
 * (at most 1,400 as text) + R246's other drum members (under 700) + the UPDATE frame and doubled quotes (under 200),
 * about 6,100. 7 KB, static (no allocation, 2,168 bytes more bss); a patch past it still fails cleanly as before. */
#define SQL_CAP 7168u
typedef int (*callback_t)(void *,int,char **,char **);
static char workspace[SQL_CAP];
static volatile uint32_t busy;
#if GIN_WORKING_SLOTS
static uint32_t working_saved[8],working_cursor;
#endif
volatile uint32_t gin_fw_save_error,gin_fw_save_count;
#ifdef OP1_HOST
extern const void *gin_save_const(const void *,const char *);
extern void *gin_save_mutable(void *,const char *);
extern void gin_save_assign(void *,const void *),gin_save_text(void *,const char *),gin_save_destroy(void *);
extern uint32_t gin_save_type(const void *);
extern int gin_save_missing(const void *);
extern const char *gin_save_chars(const void *);
extern void gin_save_write(const void *,gin_fw_string_t *);
extern void gin_save_heap_free(void *);
extern uint32_t gin_save_original_normalize(void *,uint32_t);
extern void *gin_save_patch(uint32_t);
extern void gin_save_original_read(void *,uint32_t);
extern void gin_save_string_assign(gin_fw_string_t *,const char *,uint32_t);
extern void gin_save_original_slot_save(uint32_t,const void *);
#if GIN_WORKING_SLOTS
extern void gin_save_original_tick(void);
#endif
extern void *gin_save_database_lock(void);
extern void gin_save_database_unlock(void *);
extern uint32_t gin_save_exec(void *,const char *,callback_t,void *);
static uint32_t lock(void){if(busy)return 0;busy=1;return 1;}
static void unlock(void){busy=0;}
#else

static int gin_save_missing(const void *p){return (uintptr_t)p==0x01dd4fe4u;}
static uint32_t gin_save_type(const void *p){return p?((const unsigned char *)p)[8]:0;}
static const void *gin_save_const(const void *p,const char *key){return ((const void *(*)(const void *,const char *))0x019beb70)(p,key);}
static void *gin_save_mutable(void *p,const char *key){return ((void *(*)(void *,const char *))0x019bfeb4)(p,key);}
static const char *gin_save_chars(const void *p){return *(const char *const *)p;}
static void gin_save_assign(void *a,const void *b){((void(*)(void *,const void *))0x019bddc0)(a,b);}
static void gin_save_text(void *a,const char *b){((void(*)(void *,const char *))0x019bdc4c)(a,b);}
static void gin_save_destroy(void *p){((void(*)(void *,int))0x019bdd18)(p,2);}
static void gin_save_write(const void *p,gin_fw_string_t *s){((void(*)(const void *,void *))0x019bf858)(p,s);}
static void gin_save_heap_free(void *p){((void(*)(void *))0x01a5e310)(p);}
extern uint32_t gin_fw_original_normalize(void *,uint32_t);
#define gin_save_original_normalize gin_fw_original_normalize
static void *gin_save_patch(uint32_t s){return (void *)(0x01ddd9a8u+56u*s);}
static void gin_save_original_read(void *out,uint32_t slot){((void(*)(void *,uint32_t))0x01995bac)(out,slot);}
static void gin_save_string_assign(gin_fw_string_t *out,const char *text,uint32_t n){((void(*)(void *,const char *,uint32_t))0x019bdb10)(out,text,n);}
#if GIN_WORKING_SLOTS
static void gin_save_original_tick(void){((void(*)(void))0x019ad090)();}
#endif
static void gin_save_original_slot_save(uint32_t slot,const void *s){((void(*)(uint32_t,const void *))0x01995c88)(slot,s);}
static uint32_t lock(void){uint32_t mask,result;__asm__ volatile("CLI %0;":"=d"(mask)::"memory");result=!busy;if(result)busy=1;__asm__ volatile("STI %0;"::"d"(mask):"memory");return result;}
static void unlock(void){__asm__ volatile("":::"memory");busy=0;}
static void *gin_save_database_lock(void){
 void *db=*(void *volatile *)0x01bab720,*sem=*(void *volatile *)0x01bab718;
 if(!db||!sem)return 0;
 ((void(*)(void *,int))0xffa0b6e8)(sem,0);return db;
}
static void gin_save_database_unlock(void *db){(void)db;((void(*)(void *))0xffa0b96c)(*(void *volatile *)0x01bab718);}
static uint32_t gin_save_exec(void *db,const char *sql,callback_t cb,void *arg){return ((uint32_t(*)(void *,const char *,callback_t,void *,void *))0x019c9460)(*(void **)db,sql,cb,arg,0);}
#endif




static void gin_save_string_destroy(gin_fw_string_t *s){
 if(s->cap>15u)gin_save_heap_free((void *)s->u.ptr);
 s->size=0;s->cap=15u;s->u.buf[0]=0;
}
static int equal(const char *a,const char *b){if(!a||!b)return 0;while(*a&&*a==*b){++a;++b;}return *a==*b;}
static uint32_t length(const char *s,uint32_t cap){uint32_t n=0;if(!s)return cap;while(n<cap&&s[n])++n;return n;}
static int is_gin(const void *json){const void *v;if(gin_save_type(json)!=7)return 0;v=gin_save_const(json,"type");return gin_save_type(v)==4&&equal(gin_save_chars(v),"gin");}
static int valid_text(const char *s){return length(s,GIN_SNAPSHOT_TEXT_CAPACITY)==GIN_SNAPSHOT_TEXT_BYTES&&!gin_snapshot_text_validate_readonly(s,GIN_SNAPSHOT_TEXT_BYTES);}
static void *json_for(uint32_t slot){unsigned char *p;if(slot>=8)return 0;p=gin_save_patch(slot);return p&&*(uint32_t *)p==slot?(void *)(p+4):0;}


uint32_t gin_fw_normalize(void *json,uint32_t mode){
 uint32_t value[4]={0},result;const void *field;const char *text;
 if(!is_gin(json))return gin_save_original_normalize(json,mode);
 field=gin_save_const(json,"gin_state");
 if(gin_save_missing(field))return gin_save_original_normalize(json,mode);
 text=gin_save_type(field)==4?gin_save_chars(field):0;
 if(!valid_text(text)){gin_fw_save_error=OP1_DRUM_SNAPSHOT;return 1;}
 gin_save_assign(value,field);
 result=gin_save_original_normalize(json,mode);
 if(!result)gin_save_assign(gin_save_mutable(json,"gin_state"),value);
 gin_save_destroy(value);return result;
}

void gin_fw_snapshot_loaded(uint32_t slot){
#if GIN_WORKING_SLOTS
 if(slot<8)working_saved[slot]=gin_fw_snapshot_epoch(slot);
#else
 (void)slot;
#endif
}
int gin_fw_snapshot_restore(uint32_t slot,gin_preset_owned_t *out){
 const void *json=json_for(slot),*field;const char *s;int result;
 if(!is_gin(json))return 0;
 field=gin_save_const(json,"gin_state");if(gin_save_missing(field))return 0;
 s=gin_save_type(field)==4?gin_save_chars(field):0;
 if(length(s,GIN_SNAPSHOT_TEXT_CAPACITY)!=GIN_SNAPSHOT_TEXT_BYTES||!lock())return -1;
 result=gin_snapshot_text_decode(s,GIN_SNAPSHOT_TEXT_BYTES,(unsigned char *)workspace,sizeof(workspace),out);
 unlock();if(result)gin_fw_save_error=OP1_DRUM_SNAPSHOT;return result?-1:1;
}
static int append(uint32_t *n,const char *s){while(*s){if(*n>=SQL_CAP-1)return -1;workspace[(*n)++]=*s++;}workspace[*n]=0;return 0;}




static int sql_update(uint32_t slot,const char *text){uint32_t n=0;if(append(&n,"UPDATE OR ROLLBACK drum_slots SET patch='"))return -1;while(*text){char c=*text++;if(n>=SQL_CAP-2)return -1;workspace[n++]=c;if(c=='\'')workspace[n++]=c;}workspace[n]=0;if(append(&n,"' WHERE id=")||n>SQL_CAP-3)return -1;workspace[n++]=(char)('0'+slot);workspace[n++]=';';workspace[n]=0;return 0;}
typedef struct {const char *text;uint32_t found;} readback_t;
static int readback(void *p,int columns,char **v,char **names){readback_t *r=p;(void)names;if(columns!=1||!v||!v[0]||!equal(r->text,v[0]))return 1;r->found++;return 0;}
static uint32_t store_slot(uint32_t slot,const char *text,uint32_t epoch){
 uint32_t result;void *db;char query[]="SELECT patch FROM drum_slots WHERE id=0;";readback_t check;
 if(sql_update(slot,text))return OP1_DRUM_SNAPSHOT;
 db=gin_save_database_lock();if(!db)return OP1_DRUM_IDENTITY;
 result=gin_save_exec(db,"BEGIN IMMEDIATE;",0,0);
 if(!result){
 result=gin_save_exec(db,workspace,0,0);
 if(!result){query[sizeof(query)-3]=(char)('0'+slot);check.text=text;check.found=0;result=gin_save_exec(db,query,readback,&check);if(!result&&check.found!=1)result=OP1_DRUM_SNAPSHOT;}
 if(!result&&epoch&&gin_fw_snapshot_epoch(slot)!=epoch)result=OP1_DRUM_IDENTITY;
 if(!result)result=gin_save_exec(db,"COMMIT;",0,0);
 if(result)(void)gin_save_exec(db,"ROLLBACK;",0,0);
 }
 gin_save_database_unlock(db);return result;
}
void gin_fw_snapshot_slot_save(uint32_t slot,const void *native_string){
 const gin_fw_string_t *s=native_string;const char *text;
 if(slot>=8||!(gin_fw_selected_mask&(1u<<slot))){gin_save_original_slot_save(slot,native_string);return;}
 if(!s||s->size>=SQL_CAP||!lock()){gin_fw_save_error=OP1_DRUM_SNAPSHOT;return;}
 text=s->cap>15?s->u.ptr:s->u.buf;
 gin_fw_save_error=length(text,SQL_CAP)==s->size?store_slot(slot,text,0):OP1_DRUM_SNAPSHOT;
 unlock();
}





uint32_t gin_fw_snapshot_read(void *metadata,uint32_t slot){
 uint32_t copy[4]={0},text_value[4],epoch,result=OP1_DRUM_SNAPSHOT,n;
 void *json=json_for(slot);gin_fw_string_t serialized;const char *text;
 if(!metadata||slot>=8){gin_fw_save_error=OP1_DRUM_RANGE;return OP1_DRUM_RANGE;}
 if(!is_gin(json)){gin_save_original_read(metadata,slot);return 0;}
 if(!lock()){gin_fw_save_error=OP1_DRUM_IDENTITY;return OP1_DRUM_IDENTITY;}
 if(gin_fw_snapshot_capture(slot,workspace,sizeof(workspace),&epoch))goto done;
 gin_save_assign(copy,json);gin_save_text(text_value,workspace);
 gin_save_assign(gin_save_mutable(copy,"gin_state"),text_value);gin_save_destroy(text_value);
 gin_save_write(copy,&serialized);text=serialized.cap>15?serialized.u.ptr:serialized.u.buf;n=serialized.size;


 if(!text||n>=SQL_CAP||length(text,SQL_CAP)!=n||sql_update(slot,text)||gin_fw_snapshot_epoch(slot)!=epoch)goto string_done;
 gin_save_original_read(metadata,slot);
 gin_save_string_assign((gin_fw_string_t *)metadata,text,n);
 result=0;++gin_fw_save_count;
string_done:
 gin_save_string_destroy(&serialized);
done:
 gin_save_destroy(copy);unlock();gin_fw_save_error=result;return result;
}

#if GIN_WORKING_SLOTS



uint32_t gin_fw_working_flush(uint32_t slot){
 uint32_t copy[4]={0},text_value[4],epoch,result=OP1_DRUM_SNAPSHOT;
 void *json=json_for(slot);gin_fw_string_t serialized;const char *text;uint32_t n;
 if(slot>=8||!(gin_fw_selected_mask&(1u<<slot))||!is_gin(json))return OP1_DRUM_SELECTION;
 if(!lock())return OP1_DRUM_IDENTITY;
 if(gin_fw_snapshot_capture(slot,workspace,sizeof(workspace),&epoch))goto done;
 gin_save_assign(copy,json);gin_save_text(text_value,workspace);
 gin_save_assign(gin_save_mutable(copy,"gin_state"),text_value);gin_save_destroy(text_value);
 gin_save_write(copy,&serialized);text=serialized.cap>15?serialized.u.ptr:serialized.u.buf;n=serialized.size;
 if(!text||n>=SQL_CAP||length(text,SQL_CAP)!=n)goto string_done;
 result=store_slot(slot,text,epoch);
 if(!result){gin_save_assign(json,copy);working_saved[slot]=epoch;++gin_fw_save_count;}
string_done:
 gin_save_string_destroy(&serialized);
done:
 gin_save_destroy(copy);unlock();gin_fw_save_error=result;return result;
}
void gin_fw_working_tick(void){



 gin_save_original_tick();
 if(busy)return;
 for(uint32_t i=0;i<8;i++){
  uint32_t slot=(working_cursor+i)&7u,epoch;
  if(!(gin_fw_selected_mask&(1u<<slot)))continue;
  epoch=gin_fw_snapshot_epoch(slot);
  if(!epoch||(epoch&1u)||epoch==working_saved[slot]||!is_gin(json_for(slot)))continue;
  working_cursor=(slot+1u)&7u;
  (void)gin_fw_working_flush(slot);break;
 }
}
#endif
