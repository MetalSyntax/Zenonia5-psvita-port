#pragma once
#include <stdint.h>

#include "objc.h"

extern objc_class *c_NSObject, *c_NSString, *c_NSMutableString, *c_NSNumber, *c_NSArray, *c_NSMutableArray,
    *c_NSSet, *c_NSMutableSet, *c_NSDictionary, *c_NSMutableDictionary, *c_NSData, *c_NSMutableData, *c_NSTimer,
    *c_NSDate, *c_NSNotification, *c_NSURL, *c_NSError, *c_NSThread;

void foundation_init(void);          // define las clases nativas (antes de macho_load)
void foundation_fix_cfstrings(void); // después de macho_load

id ns_string_new(const char *utf8); // +1
id ns_str(const char *utf8);        // autoreleased
const char *ns_cstr(id s);          // "" si no es NSString
id ns_num_int(int64_t v);
id ns_array_new(void); // +1, mutable
void ns_array_add(id a, id o);
int ns_array_count(id a);
id ns_array_get(id a, int i);
id ns_dict_new(void); // +1, mutable
void ns_dict_set(id d, id k, id v);
id ns_dict_get(id d, id k);
id ns_dict_get_c(id d, const char *k);
id plist_load(const char *path); // plist binario -> objetos (+1)
void ns_post_notification(const char *name, id object, id userInfo);
double ns_run_timers(void); // dispara timers vencidos; segundos hasta el próximo
double ns_now(void);
const char *ns_language(void);
void objc_free(id o);
