#include "launcher/tools.h"
#include "launcher/i18n.h"
#include "launcher/input.h"
#include "launcher/view.h"
#include "launcher/preferences.h"
#include "launcher/timezones.h"
#include "sessiond/document.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#define LIST_ROWS 7U
#define ACTIVITY_ROWS 4U
static size_t page_rows(const struct cl_tools *t)
{
    return t->snapshot.kind == CL_TOOL_ACTIVITY || t->snapshot.kind == CL_TOOL_DEVICE ? ACTIVITY_ROWS : LIST_ROWS;
}
static enum cl_tool_operation load_operation(enum cl_tool_kind kind)
{
    if (kind == CL_TOOL_LAN_SERVICES) return CL_TOOL_LOAD_LAN_SERVICES;
    if (kind == CL_TOOL_GALLERY) return CL_TOOL_LOAD_GALLERY;
    if (kind == CL_TOOL_BATTERY) return CL_TOOL_LOAD_BATTERY;
    if (kind == CL_TOOL_FILES) return CL_TOOL_LOAD_FILES;
    if (kind == CL_TOOL_ICON_EDITOR) return CL_TOOL_LOAD_ICON_SLOTS;
    if (kind == CL_TOOL_ICON_PACKS) return CL_TOOL_LOAD_ICON_PACKS;
    if(kind==CL_TOOL_CLOCK)return CL_TOOL_LOAD_CLOCK;
    if (kind == CL_TOOL_CORES) return CL_TOOL_LOAD_CORES;
    if (kind == CL_TOOL_FILE_TRANSFER) return CL_TOOL_LOAD_FILE_TRANSFER;
    return kind == CL_TOOL_PACKAGES ? CL_TOOL_LOAD_PACKAGES :
           (kind == CL_TOOL_ACTIVITY ? CL_TOOL_LOAD_ACTIVITY :
            (kind == CL_TOOL_NETWORK ? CL_TOOL_LOAD_NETWORK :
             (kind == CL_TOOL_DEVICE ? CL_TOOL_LOAD_DEVICE :
              (kind == CL_TOOL_SCRAPER ? CL_TOOL_LOAD_SCRAPER :
               (kind == CL_TOOL_DOWNLOADS ? CL_TOOL_LOAD_DOWNLOADS : CL_TOOL_LOAD_THEMES)))));
}
void cl_tools_open(struct cl_tools *t, enum cl_tool_kind kind, struct cl_tool_request *r)
{
    memset(t,0,sizeof(*t)); memset(r,0,sizeof(*r));
    t->snapshot.kind=kind; t->waiting=true; t->group=CL_TOOL_VERIFIED;
    r->operation=load_operation(kind);
    t->last_operation=r->operation;
}
static bool text_valid(const char *text, size_t size)
{
    size_t used=strnlen(text,size);
    return used<size && cs_utf8_valid(text,used);
}
bool cl_tools_accept(struct cl_tools *t, const struct cl_tool_snapshot *s)
{
    uint64_t seen=0U;
    if (s==NULL || t==NULL || (unsigned int)s->kind>CL_TOOL_BATTERY || s->kind!=t->snapshot.kind || s->count>CL_TOOL_ROWS ||
        !text_valid(s->status,sizeof(s->status)) || !text_valid(s->selected_theme,sizeof(s->selected_theme)) ||
        !text_valid(s->icon_pack,sizeof(s->icon_pack)) ||
        !text_valid(s->icon_theme,sizeof(s->icon_theme))) return false;
    if (s->kind == CL_TOOL_GALLERY && (s->available_mask > CL_GALLERY_MAX ||
        s->enabled_mask > s->available_mask || s->count > s->available_mask - s->enabled_mask)) return false;
    if (s->kind == CL_TOOL_BATTERY) {
        if (s->battery.count > CL_BATTERY_SAMPLES) return false;
        for (size_t i = 0U; i < s->battery.count; ++i) {
            const struct cl_battery_point *p = &s->battery.points[i];
            if (p->percent > 100U || p->power > CL_POWER_UNKNOWN ||
                (i > 0U && p->seconds <= s->battery.points[i - 1U].seconds)) return false;
        }
    }
    for (size_t i=0U;i<s->count;++i) {
        const struct cl_tool_row *row=&s->rows[i];
        if (!text_valid(row->id,sizeof(row->id)) || !text_valid(row->label,sizeof(row->label)) ||
            !text_valid(row->detail,sizeof(row->detail))) return false;
        if (s->kind==CL_TOOL_PACKAGES) {
            if (row->index>=64U || (unsigned int)row->group>CL_TOOL_EXPERT || (seen & (UINT64_C(1)<<row->index))!=0U) return false;
            seen|=UINT64_C(1)<<row->index;
        }
    }
    {
        size_t selected = t->selected, first = t->first;
        bool preserve = s->kind == CL_TOOL_LAN_SERVICES ||
            (s->kind == CL_TOOL_NETWORK && t->last_operation == CL_TOOL_LOAD_NETWORK);
        uint64_t icon_group = t->snapshot.available_mask;
        t->snapshot=*s; t->desired=s->enabled_mask; t->waiting=false; t->committing=false;
        if (s->kind == CL_TOOL_ICON_EDITOR && s->error) t->snapshot.available_mask = icon_group;
        if(s->kind==CL_TOOL_CLOCK){tzset();t->timezone=s->available_mask<CL_TIMEZONE_COUNT?(unsigned int)s->available_mask:0U;}
        t->selected = preserve && selected < s->count ? selected : 0U;
        if (s->kind == CL_TOOL_GALLERY && s->count != 0U && t->gallery_target >= s->enabled_mask) {
            size_t row = t->gallery_target - (size_t)s->enabled_mask;
            t->selected = row < s->count ? row : s->count - 1U;
        }
        t->first = preserve && first <= t->selected ? first : 0U;
    }
    return true;
}
static size_t visible(const struct cl_tools *t, size_t indices[CL_TOOL_ROWS])
{
    size_t count=0U;
    for (size_t i=0U;i<t->snapshot.count;++i)
        if (t->snapshot.kind!=CL_TOOL_PACKAGES || t->snapshot.rows[i].group==t->group) indices[count++]=i;
    return count;
}
static void set_row(struct cl_tools *t, const struct cl_tool_row *row, bool enabled)
{
    uint64_t bit=UINT64_C(1)<<row->index;
    if ((row->required && !enabled) || (!row->available && enabled)) return;
    if (enabled) t->desired|=bit; else t->desired&=~bit;
}
static void move(struct cl_tools *t, size_t count, int direction, size_t amount)
{
    if (count==0U) {t->selected=t->first=0U;return;}
    if (direction<0) t->selected=t->selected>=amount?t->selected-amount:count-1U;
    else t->selected=t->selected+amount<count?t->selected+amount:0U;
    if (t->selected<t->first) t->first=t->selected;
    if (t->selected>=t->first+page_rows(t)) t->first=t->selected-page_rows(t)+1U;
}
static enum cl_tool_action request(struct cl_tools *t, struct cl_tool_request *r,
                                   enum cl_tool_operation op)
{
    memset(r,0,sizeof(*r));r->operation=op;r->revision=t->snapshot.revision;r->enabled_mask=t->desired;
    t->last_operation=op;
    strcpy(r->target,t->target);
    t->waiting=true;t->committing=op==CL_TOOL_APPLY_PACKAGES || op==CL_TOOL_ROLLBACK_PACKAGES || op==CL_TOOL_APPLY_THEME || op==CL_TOOL_APPLY_ICON_PACK || op==CL_TOOL_APPLY_ICON ||
        op==CL_TOOL_CONNECT_NETWORK || op==CL_TOOL_FORGET_NETWORK || op==CL_TOOL_SAVE_ONLINE_SETTING ||
        op==CL_TOOL_SET_CORE || op==CL_TOOL_SET_CLOCK || op==CL_TOOL_SYNC_CLOCK || op==CL_TOOL_SET_TIMEZONE;
    return CL_TOOL_REQUEST;
}
static enum cl_tool_action network_connect(struct cl_tools *t, struct cl_tool_request *r)
{
    if (t->snapshot.kind==CL_TOOL_SCRAPER || t->snapshot.kind==CL_TOOL_DOWNLOADS) {
        if (t->snapshot.kind==CL_TOOL_SCRAPER && t->edit_field==8U) {
            size_t length=strlen(t->password);
            bool valid=length>0U && length<=10U && t->password[0]>='1' && t->password[0]<='9';
            for(size_t i=1U;i<length;++i)if(t->password[i]<'0'||t->password[i]>'9')valid=false;
            if(!valid){strcpy(t->keyboard_error,CL_TR("Use a numeric game ID: 1-10 digits. X deletes."));return CL_TOOL_NO_ACTION;}
            (void)request(t,r,CL_TOOL_SCRAPE_GAME);
            strcpy(r->id,t->password);memset(t->password,0,sizeof(t->password));t->password_edit=false;
            return CL_TOOL_REQUEST;
        }
        (void)request(t,r,CL_TOOL_SAVE_ONLINE_SETTING);r->enabled_mask=t->edit_field;
        strcpy(r->id,t->password);memset(t->password,0,sizeof(t->password));t->password_edit=false;
        return CL_TOOL_REQUEST;
    }
    const char *mac = t->snapshot.rows[t->network_row].id;
    uint64_t address = 0U;
    if (strlen(mac) != 17U) return CL_TOOL_NO_ACTION;
    for (size_t i = 0U; i < 17U; ++i) {
        unsigned int digit;
        if (i % 3U == 2U) { if (mac[i] != ':') return CL_TOOL_NO_ACTION; continue; }
        if (mac[i] >= '0' && mac[i] <= '9') digit = (unsigned int)(mac[i] - '0');
        else if (mac[i] >= 'a' && mac[i] <= 'f') digit = (unsigned int)(mac[i] - 'a' + 10);
        else if (mac[i] >= 'A' && mac[i] <= 'F') digit = (unsigned int)(mac[i] - 'A' + 10);
        else return CL_TOOL_NO_ACTION;
        address = (address << 4U) | digit;
    }
    if (t->snapshot.rows[t->network_row].required && strlen(t->password) < 8U) {
        (void)snprintf(t->keyboard_error, sizeof(t->keyboard_error), CL_TR("Use at least 8 characters for this network."));
        return CL_TOOL_NO_ACTION;
    }
    (void)request(t, r, CL_TOOL_CONNECT_NETWORK);
    r->enabled_mask = address;
    strcpy(r->id, t->password);
    memset(t->password, 0, sizeof(t->password)); t->password_edit = false;
    return CL_TOOL_REQUEST;
}
/* Three compact pages cover every printable ASCII character. Only the
 * letter page changes case; symbol pages keep their exact literal characters. */
static char keyboard_character(const struct cl_tools *t, size_t index)
{
    static const char pages[3][31] = {
        "qwertyuiopasdfghjkl@zxcvbnm._-",
        "1234567890!@#$%^&*()-_=+[]{}\\|",
        "`~;:'\",.<>?/\\|+-=_()0123456789"
    };
    char c;
    if (t->keyboard_page >= 3U || index >= 30U) return '\0';
    c = pages[t->keyboard_page][index];
    if (t->keyboard_page == 0U && t->keyboard_upper && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    return c;
}
static void keyboard_begin(struct cl_tools *t)
{
    t->keyboard_cursor = 0U; t->keyboard_page = 0U; t->keyboard_upper = false;
    t->keyboard_error[0] = '\0'; t->password_edit = true;
}
static enum cl_tool_action keyboard_input(struct cl_tools *t, uint32_t keys,
                                           struct cl_tool_request *r)
{
    size_t length = strlen(t->password), cursor = t->keyboard_cursor;
    bool type = (keys & CL_KEY(CL_A)) != 0U;
    if (cursor >= 35U) cursor = 0U;
    if ((keys & CL_KEY(CL_B)) != 0U) {
        memset(t->password, 0, sizeof(t->password)); t->password_edit = false;
        t->keyboard_error[0] = '\0'; return CL_TOOL_NO_ACTION;
    }
    if ((keys & CL_KEY(CL_Y)) != 0U) t->password_visible = !t->password_visible;
    if ((keys & CL_KEY(CL_SELECT)) != 0U) t->keyboard_upper = !t->keyboard_upper;
    if ((keys & CL_KEY(CL_L1)) != 0U) t->keyboard_page = (t->keyboard_page + 2U) % 3U;
    if ((keys & CL_KEY(CL_R1)) != 0U) t->keyboard_page = (t->keyboard_page + 1U) % 3U;
    if ((keys & CL_KEY(CL_UP)) != 0U)
        cursor = cursor >= 30U ? 20U + 2U * (cursor - 30U) :
                 (cursor >= 10U ? cursor - 10U : 30U + cursor / 2U);
    if ((keys & CL_KEY(CL_DOWN)) != 0U)
        cursor = cursor >= 30U ? 2U * (cursor - 30U) :
                 (cursor >= 20U ? 30U + (cursor - 20U) / 2U : cursor + 10U);
    if ((keys & (CL_KEY(CL_LEFT) | CL_KEY(CL_RIGHT))) != 0U) {
        size_t first = cursor >= 30U ? 30U : cursor / 10U * 10U;
        size_t count = cursor >= 30U ? 5U : 10U;
        size_t step = (keys & CL_KEY(CL_LEFT)) != 0U ? count - 1U : 1U;
        cursor = first + (cursor - first + step) % count;
    }
    t->keyboard_cursor = cursor;
    if ((keys & CL_KEY(CL_START)) != 0U || (type && cursor == 34U))
        return network_connect(t, r);
    if ((keys & CL_KEY(CL_X)) != 0U || (type && cursor == 33U)) {
        if (length > 0U) t->password[length - 1U] = '\0';
        t->keyboard_error[0] = '\0';
    } else if (type && cursor == 30U) {
        t->keyboard_upper = !t->keyboard_upper;
        t->keyboard_page = 0U;
    } else if (type && cursor == 31U) {
        t->keyboard_page = (t->keyboard_page + 1U) % 3U;
    } else if (type && (cursor < 30U || cursor == 32U)) {
        char c = cursor == 32U ? ' ' : keyboard_character(t, cursor);
        if (length < sizeof(t->password) - 1U && c != '\0') {
            t->password[length] = c; t->password[length + 1U] = '\0';
            t->keyboard_error[0] = '\0';
        } else {
            (void)snprintf(t->keyboard_error, sizeof(t->keyboard_error), CL_TR("Maximum 63 characters. X deletes."));
        }
    }
    return CL_TOOL_NO_ACTION;
}
enum cl_tool_action cl_tools_input(struct cl_tools *t, uint32_t keys, struct cl_tool_request *r)
{
    size_t indices[CL_TOOL_ROWS],count;
    bool packages=t->snapshot.kind==CL_TOOL_PACKAGES;
    if (t->password_edit) return keyboard_input(t,keys,r);
    if (t->snapshot.kind == CL_TOOL_BATTERY) {
        if ((keys & (CL_KEY(CL_B) | CL_KEY(CL_MENU))) != 0U) return CL_TOOL_CLOSE;
        if (t->waiting) return CL_TOOL_NO_ACTION;
        if ((keys & CL_KEY(CL_A)) != 0U) return request(t,r,CL_TOOL_LOAD_BATTERY);
        if ((keys & (CL_KEY(CL_R1) | CL_KEY(CL_R2))) != 0U && t->battery_zoom < 2U) {
            ++t->battery_zoom;
            t->battery_back_hours = 0U;
        }
        if ((keys & (CL_KEY(CL_L1) | CL_KEY(CL_L2))) != 0U && t->battery_zoom > 0U) {
            --t->battery_zoom;
            t->battery_back_hours = 0U;
        }
        unsigned int limit = 24U - (16U >> t->battery_zoom);
        if ((keys & CL_KEY(CL_LEFT)) != 0U && t->battery_back_hours < limit) ++t->battery_back_hours;
        if ((keys & CL_KEY(CL_RIGHT)) != 0U && t->battery_back_hours > 0U) --t->battery_back_hours;
        return CL_TOOL_NO_ACTION;
    }
    if(t->snapshot.kind==CL_TOOL_GALLERY){
        if((keys&CL_KEY(CL_MENU))!=0U ||
           ((t->waiting || t->snapshot.count==0U) && (keys&(CL_KEY(CL_A)|CL_KEY(CL_B)))!=0U)) return CL_TOOL_CLOSE;
        if(t->waiting || !t->snapshot.ready) return CL_TOOL_NO_ACTION;
        if((keys&CL_KEY(CL_Y))!=0U) t->summary=!t->summary;
        bool next = (keys & (CL_KEY(CL_RIGHT) | CL_KEY(CL_A))) != 0U;
        bool previous = (keys & (CL_KEY(CL_LEFT) | CL_KEY(CL_B))) != 0U;
        size_t absolute = (size_t)t->snapshot.enabled_mask + t->selected;
        if (next == previous) return CL_TOOL_NO_ACTION;
        if (next && absolute + 1U >= t->snapshot.available_mask)
            return (keys & CL_KEY(CL_A)) != 0U ? CL_TOOL_CLOSE : CL_TOOL_NO_ACTION;
        if (previous && absolute == 0U)
            return (keys & CL_KEY(CL_B)) != 0U ? CL_TOOL_CLOSE : CL_TOOL_NO_ACTION;
        t->gallery_target = next ? absolute + 1U : absolute - 1U;
        if (t->gallery_target < t->snapshot.enabled_mask ||
            t->gallery_target >= t->snapshot.enabled_mask + t->snapshot.count) {
            enum cl_tool_action action = request(t, r, CL_TOOL_LOAD_GALLERY);
            r->enabled_mask = t->gallery_target;
            return action;
        }
        t->selected = t->gallery_target - (size_t)t->snapshot.enabled_mask;
        return CL_TOOL_NO_ACTION;
    }
    if (t->snapshot.kind==CL_TOOL_NETWORK && t->confirm_exit) {
        if ((keys&CL_KEY(CL_B))!=0U) t->confirm_exit=false;
        if ((keys&CL_KEY(CL_A))!=0U) {t->confirm_exit=false;return request(t,r,CL_TOOL_FORGET_NETWORK);}
        return CL_TOOL_NO_ACTION;
    }
    if ((keys&CL_KEY(CL_B))!=0U) {
        if (t->committing) return CL_TOOL_NO_ACTION;
        if(t->snapshot.kind==CL_TOOL_FILES && !t->waiting && t->snapshot.enabled_mask!=0U)
            return request(t,r,CL_TOOL_PARENT_DIRECTORY);
        t->batch_scrape=false;
        if (packages && t->summary) {t->summary=false;return CL_TOOL_NO_ACTION;}
        memset(t->password,0,sizeof(t->password));return CL_TOOL_CLOSE;
    }
    if (t->waiting) return CL_TOOL_NO_ACTION;
    if (t->snapshot.kind == CL_TOOL_ICON_EDITOR && t->snapshot.error &&
        (keys & CL_KEY(CL_A)) != 0U) {
        enum cl_tool_operation reload = t->target[0] != '\0' ?
            CL_TOOL_LOAD_ICON_CHOICES : CL_TOOL_LOAD_ICON_SLOTS;
        enum cl_tool_action action = request(t, r, reload);
        r->enabled_mask = t->snapshot.available_mask;
        return action;
    }
    if (t->snapshot.error) return (keys&CL_KEY(CL_A))!=0U?
        (t->last_operation == CL_TOOL_RESET_CONFIG ? CL_TOOL_CLOSE :
         request(t,r,t->snapshot.kind==CL_TOOL_MAINTENANCE?t->last_operation:load_operation(t->snapshot.kind))):
        CL_TOOL_NO_ACTION;
    if (!t->snapshot.ready) return CL_TOOL_NO_ACTION;
    if ((t->snapshot.kind==CL_TOOL_SCRAPER || t->snapshot.kind==CL_TOOL_DOWNLOADS) &&
        (keys&CL_KEY(CL_SELECT))!=0U) {t->summary=!t->summary;return CL_TOOL_NO_ACTION;}
    if ((t->snapshot.kind==CL_TOOL_SCRAPER || t->snapshot.kind==CL_TOOL_DOWNLOADS) && t->summary)
        return CL_TOOL_NO_ACTION;
    if (t->snapshot.kind==CL_TOOL_NETWORK && (keys&CL_KEY(CL_X))!=0U)
        return request(t,r,CL_TOOL_LOAD_NETWORK);
    if (packages && t->summary) {
        if ((keys&(CL_KEY(CL_A)|CL_KEY(CL_START)))!=0U)
            return t->desired==t->snapshot.enabled_mask?CL_TOOL_CLOSE:request(t,r,CL_TOOL_APPLY_PACKAGES);
        if ((keys&CL_KEY(CL_Y))!=0U) return request(t,r,CL_TOOL_ROLLBACK_PACKAGES);
        return CL_TOOL_NO_ACTION;
    }
    if (packages && (keys&CL_KEY(CL_START))!=0U) {
        t->summary=true;
        return CL_TOOL_NO_ACTION;
    }
    if (packages && (keys&(CL_KEY(CL_L1)|CL_KEY(CL_R1)))!=0U) {
        unsigned int step=(keys&CL_KEY(CL_R1))!=0U?1U:2U;
        t->group=(enum cl_tool_group)(((unsigned int)t->group+step)%3U);
        t->selected=t->first=0U;
    }
    count=visible(t,indices);
    if (packages && count!=0U && (keys&(CL_KEY(CL_L2)|CL_KEY(CL_R2)))!=0U) {
        size_t pages=(count+LIST_ROWS-1U)/LIST_ROWS;
        size_t step=(keys&CL_KEY(CL_R2))!=0U?1U:pages-1U;
        size_t page=(t->selected/LIST_ROWS+step)%pages;
        t->selected=t->first=page*LIST_ROWS;
        return CL_TOOL_NO_ACTION;
    }
    if ((keys&CL_KEY(CL_UP))!=0U) move(t,count,-1,1U);
    if ((keys&CL_KEY(CL_DOWN))!=0U) move(t,count,1,1U);
    if (!packages) {
        if(t->snapshot.kind==CL_TOOL_FILES){
            if((keys&CL_KEY(CL_L1))!=0U) move(t,count,-1,LIST_ROWS);
            if((keys&CL_KEY(CL_R1))!=0U) move(t,count,1,LIST_ROWS);
            if(count!=0U && (keys&CL_KEY(CL_A))!=0U && t->snapshot.rows[t->selected].required){
                enum cl_tool_action action=request(t,r,CL_TOOL_ENTER_DIRECTORY);
                strcpy(r->id,t->snapshot.rows[t->selected].id);
                return action;
            }
            return CL_TOOL_NO_ACTION;
        }
        if(t->snapshot.kind==CL_TOOL_CLOCK){
            if((keys&CL_KEY(CL_X))!=0U)return request(t,r,CL_TOOL_LOAD_CLOCK);
            if(t->selected<5U && (keys&(CL_KEY(CL_LEFT)|CL_KEY(CL_RIGHT)))!=0U){
                time_t stamp=(time_t)t->desired;struct tm value;
                if(localtime_r(&stamp,&value)!=NULL){int direction=(keys&CL_KEY(CL_RIGHT))!=0U?1:-1;
                    if(t->selected==0U)value.tm_year+=direction;else if(t->selected==1U)value.tm_mon+=direction;
                    else if(t->selected==2U)value.tm_mday+=direction;else if(t->selected==3U)value.tm_hour+=direction;else value.tm_min+=direction;
                    value.tm_isdst=-1;stamp=mktime(&value);if(stamp>=(time_t)946684800 && stamp<=(time_t)4102444799LL)t->desired=(uint64_t)stamp;
                }
            }
            if(t->selected==5U && (keys&CL_KEY(CL_A))!=0U){enum cl_tool_action action=request(t,r,CL_TOOL_SET_CLOCK);r->enabled_mask=t->desired;return action;}
            if(t->selected==6U && (keys&CL_KEY(CL_A))!=0U)return request(t,r,CL_TOOL_SYNC_CLOCK);
            if(t->selected==7U){
                if((keys&CL_KEY(CL_LEFT))!=0U)t->timezone=(unsigned int)((t->timezone+CL_TIMEZONE_COUNT-1U)%CL_TIMEZONE_COUNT);
                if((keys&CL_KEY(CL_RIGHT))!=0U)t->timezone=(unsigned int)((t->timezone+1U)%CL_TIMEZONE_COUNT);
                if((keys&CL_KEY(CL_A))!=0U){enum cl_tool_action action=request(t,r,CL_TOOL_SET_TIMEZONE);r->enabled_mask=t->timezone;return action;}
            }
            return CL_TOOL_NO_ACTION;
        }
        if ((keys&CL_KEY(CL_LEFT))!=0U) move(t,count,-1,page_rows(t));
        if ((keys&CL_KEY(CL_RIGHT))!=0U) move(t,count,1,page_rows(t));
        if (t->snapshot.kind==CL_TOOL_ACTIVITY && (keys&CL_KEY(CL_Y))!=0U) {
            t->raw_activity=!t->raw_activity;enum cl_tool_action action=request(t,r,CL_TOOL_LOAD_ACTIVITY);
            if(t->raw_activity)strcpy(r->id,"raw");return action;
        }
        if (t->snapshot.kind == CL_TOOL_ICON_EDITOR && count > 0U &&
            (keys & CL_KEY(CL_A)) != 0U && t->snapshot.rows[indices[t->selected]].available) {
            const struct cl_tool_row *row = &t->snapshot.rows[indices[t->selected]];
            bool choice = t->snapshot.enabled_mask != 0U;
            enum cl_tool_action action = request(t, r, choice ? CL_TOOL_APPLY_ICON : CL_TOOL_LOAD_ICON_CHOICES);
            if (!choice) strcpy(t->target, row->id);
            else strcpy(r->id, row->id);
            strcpy(r->target, t->target);
            r->enabled_mask = t->snapshot.available_mask;
            return action;
        }
        if ((t->snapshot.kind==CL_TOOL_THEMES || t->snapshot.kind==CL_TOOL_ICON_PACKS) && count>0U && (keys&CL_KEY(CL_A))!=0U && t->snapshot.rows[indices[t->selected]].available) {
            enum cl_tool_action action=request(t,r,t->snapshot.kind==CL_TOOL_ICON_PACKS ? CL_TOOL_APPLY_ICON_PACK : CL_TOOL_APPLY_THEME);
            strcpy(r->id,t->snapshot.rows[indices[t->selected]].id);return action;
        }
        if (t->snapshot.kind==CL_TOOL_CORES && count>0U && (keys&CL_KEY(CL_A))!=0U) {
            const struct cl_tool_row *row=&t->snapshot.rows[indices[t->selected]];
            if (!row->available) return CL_TOOL_NO_ACTION;
            enum cl_tool_action action=request(t,r,CL_TOOL_SET_CORE);
            if (strcmp(row->id,"@default") != 0) strcpy(r->id,row->id);
            return action;
        }
        if (t->snapshot.kind==CL_TOOL_LAN_SERVICES) {
            if ((keys&CL_KEY(CL_X))!=0U) return request(t,r,CL_TOOL_LOAD_LAN_SERVICES);
            if (count>0U && (keys&CL_KEY(CL_A))!=0U) {
                const char *id=t->snapshot.rows[indices[t->selected]].id;
                if (strcmp(id,"smb")==0) return request(t,r,CL_TOOL_TOGGLE_LAN_SMB);
                if (strcmp(id,"sftp")==0) return request(t,r,CL_TOOL_TOGGLE_LAN_SFTP);
            }
        }
        if (t->snapshot.kind==CL_TOOL_FILE_TRANSFER && (keys&CL_KEY(CL_X))!=0U)
            return request(t,r,CL_TOOL_LOAD_FILE_TRANSFER);
        if (t->snapshot.kind==CL_TOOL_FILE_TRANSFER && count>0U && (keys&CL_KEY(CL_A))!=0U) {
            const struct cl_tool_row *row=&t->snapshot.rows[indices[t->selected]];
            if (!row->available) return CL_TOOL_NO_ACTION;
            return request(t,r,strcmp(row->id,"start")==0 ? CL_TOOL_START_FILE_TRANSFER :
                strcmp(row->id,"stop")==0 ? CL_TOOL_STOP_FILE_TRANSFER :
                strcmp(row->id,"usb")==0 ? CL_TOOL_START_USB_TRANSFER :
                strcmp(row->id,"import")==0 ? CL_TOOL_IMPORT_USB_STORAGE : CL_TOOL_LOAD_FILE_TRANSFER);
        }
        if (t->snapshot.kind==CL_TOOL_NETWORK && count>0U && (keys&CL_KEY(CL_A))!=0U) {
            const struct cl_tool_row *row=&t->snapshot.rows[indices[t->selected]];
            if(!row->available)return CL_TOOL_NO_ACTION;
            if(strcmp(row->id,"@scan")==0)return request(t,r,CL_TOOL_SCAN_NETWORK);
            if(strcmp(row->id,"@refresh")==0)return request(t,r,CL_TOOL_LOAD_NETWORK);
            if(strcmp(row->id,"@disconnect")==0)return request(t,r,CL_TOOL_DISCONNECT_NETWORK);
            if(strcmp(row->id,"@reconnect")==0)return request(t,r,CL_TOOL_RECONNECT_NETWORK);
            if(strcmp(row->id,"@forget")==0){t->confirm_exit=true;return CL_TOOL_NO_ACTION;}
            t->network_row=indices[t->selected];memset(t->password,0,sizeof(t->password));
            t->password_visible=false;
            if(!row->required)return network_connect(t,r);
            keyboard_begin(t);
        }
        if ((t->snapshot.kind==CL_TOOL_SCRAPER || t->snapshot.kind==CL_TOOL_DOWNLOADS) &&
            count>0U && (keys&CL_KEY(CL_A))!=0U) {
            const struct cl_tool_row *row=&t->snapshot.rows[indices[t->selected]];
            if(!row->available)return CL_TOOL_NO_ACTION;
            if(strcmp(row->id,"@selected")==0)return request(t,r,CL_TOOL_SCRAPE_GAME);
            if(strcmp(row->id,"@gameid")==0){
                t->edit_field=8U;t->network_row=indices[t->selected];memset(t->password,0,sizeof(t->password));
                t->password_visible=true;keyboard_begin(t);t->keyboard_page=1U;return CL_TOOL_NO_ACTION;
            }
            if(strcmp(row->id,"@missing")==0){t->batch_scrape=true;enum cl_tool_action action=request(t,r,CL_TOOL_SCRAPE_MISSING);r->revision=0U;r->enabled_mask=0U;return action;}
            if(row->id[0]!='@'){
                enum cl_tool_action action=request(t,r,CL_TOOL_DOWNLOAD_PACKAGE);strcpy(r->id,row->id);return action;
            }
            t->edit_field=strcmp(row->id,"@user")==0?1U:(strcmp(row->id,"@password")==0?2U:
                (strcmp(row->id,"@devid")==0?3U:(strcmp(row->id,"@devpassword")==0?4U:
                 (strcmp(row->id,"@repository")==0?5U:(strcmp(row->id,"@region")==0?6U:7U)))));
            if(t->edit_field>=6U){(void)request(t,r,CL_TOOL_SAVE_ONLINE_SETTING);r->enabled_mask=t->edit_field;return CL_TOOL_REQUEST;}
            t->network_row=indices[t->selected];memset(t->password,0,sizeof(t->password));
            t->password_visible=t->edit_field!=2U&&t->edit_field!=4U;keyboard_begin(t);
        }
        return CL_TOOL_NO_ACTION;
    }
    if ((keys&CL_KEY(CL_X))!=0U) {
        bool all_on=true,all_off=true,roms=false;
        for(size_t i=0U;i<count;++i){const struct cl_tool_row *row=&t->snapshot.rows[indices[i]];if(row->required)continue;
            bool on=(t->desired&(UINT64_C(1)<<row->index))!=0U;all_on=all_on&&on;all_off=all_off&&!on;roms=roms||row->has_roms;}
        for(size_t i=0U;i<count;++i){const struct cl_tool_row *row=&t->snapshot.rows[indices[i]];
            set_row(t,row,all_on?false:(all_off&&roms?row->has_roms:true));}
    }
    if ((keys&CL_KEY(CL_Y))!=0U) {
        t->desired=t->snapshot.enabled_mask;
        return CL_TOOL_NO_ACTION;
    }
    if (count>0U && (keys&(CL_KEY(CL_A)|CL_KEY(CL_LEFT)|CL_KEY(CL_RIGHT)))!=0U) {
        const struct cl_tool_row *row=&t->snapshot.rows[indices[t->selected]];
        bool enabled=(t->desired&(UINT64_C(1)<<row->index))!=0U;
        if((keys&CL_KEY(CL_LEFT))!=0U)enabled=false;else if((keys&CL_KEY(CL_RIGHT))!=0U)enabled=true;else enabled=!enabled;
        set_row(t,row,enabled);
    }
    return CL_TOOL_NO_ACTION;
}
static void tool_hint(SDL_Surface *surface,const struct cl_view *v,int x,int y,
                      const char *label,uint32_t color,int width)
{
    if(!v->theme->hide_hints && (v->appearance&CL_HIDE_HINT_LABELS)==0U)
        cl_view_text(surface,v->assets,CL_FONT_HINT,x,y,label,color,width);
}

static uint32_t battery_estimate(const struct cl_battery_history *history, uint32_t now)
{
    if (history->count < 3U) return 0U;
    size_t last = history->count - 1U;
    const struct cl_battery_point *latest = &history->points[last];
    if (latest->power != CL_ON_BATTERY || latest->seconds > now || now - latest->seconds > 600U) return 0U;
    size_t first = last;
    while (first > 0U) {
        const struct cl_battery_point *prev = &history->points[first - 1U];
        const struct cl_battery_point *next = &history->points[first];
        if (prev->power != CL_ON_BATTERY || next->seconds - prev->seconds > 600U ||
            latest->seconds - prev->seconds > 3600U) break;
        --first;
    }
    const struct cl_battery_point *start = &history->points[first];
    uint32_t duration = latest->seconds - start->seconds;
    int decrease = (int)start->percent - (int)latest->percent;
    if (duration < 600U || decrease < 2) return 0U;
    uint32_t estimate = duration * latest->percent / (uint32_t)decrease;
    return estimate <= 86400U ? estimate : 0U;
}

static void battery_draw(SDL_Surface *s, const struct cl_view *v, const struct cl_tools *t)
{
    const struct cl_battery_history *history = &t->snapshot.battery;
    const uint32_t color = v->theme->fonts[CL_FONT_LIST].color;
    const uint32_t span = (16U >> t->battery_zoom) * 3600U;
    time_t wall = time(NULL);
    uint32_t now = wall > 0 && (uint64_t)wall <= UINT32_MAX ? (uint32_t)wall : 0U;
    uint32_t end = now > t->battery_back_hours * 3600U ? now - t->battery_back_hours * 3600U : 0U;
    uint32_t start = end > span ? end - span : 0U;
    char text[96];
    cl_view_frame(s,v,CL_TR("Battery Monitor"));
    if (v->battery >= 0)
        (void)snprintf(text,sizeof(text),"%d%%  %s",v->battery,v->external_power ? CL_TR("Charging") : CL_TR("Current charge"));
    else strcpy(text,CL_TR("Live battery reading unavailable"));
    cl_view_text(s,v->assets,CL_FONT_LIST,24,75,text,color,590);
    if (!t->snapshot.ready || history->count == 0U) {
        const char *status = t->waiting ? "Reading battery history..." : t->snapshot.error ?
            t->snapshot.status : "Waiting for battery history";
        cl_view_text(s,v->assets,CL_FONT_LIST,24,205,status,color,590);
    } else {
        static const char *const labels[] = {"100%", "75%", "50%", "25%", "0%"};
        for (int row = 0; row < 5; ++row) {
            int y = 150 + row * 51;
            cl_view_text(s,v->assets,CL_FONT_HINT,15,y-7,labels[row],v->theme->muted,55);
            cl_round_rect(s,(SDL_Rect){70,y,536,1},0,v->theme->panel);
        }
        for (size_t i = 0U; i < history->count; ++i) {
            const struct cl_battery_point *p = &history->points[i];
            if (p->seconds < start || p->seconds > end) continue;
            int x = 70 + (int)(((uint64_t)p->seconds - start) * 533U / span);
            int y = 354 - (int)((unsigned int)p->percent * 204U / 100U);
            uint32_t ink = p->power == CL_CHARGING ? 0x76e8a2U : p->power == CL_ON_BATTERY ? 0x5eb8ffU : v->theme->muted;
            cl_round_rect(s,(SDL_Rect){x,y,3,3},0,ink);
        }
        uint32_t estimate = v->external_power ? 0U : battery_estimate(history, now);
        if (estimate > 0U)
            (void)snprintf(text,sizeof(text),CL_TR("Estimated remaining %uh %02um"),estimate / 3600U, (estimate / 60U) % 60U);
        else strcpy(text,CL_TR("Estimate available after sustained discharge"));
        cl_view_text(s,v->assets,CL_FONT_DETAIL,24,380,text,color,590);
    }
    (void)snprintf(text,sizeof(text),"%u hour view  |  %s",span / 3600U,t->battery_back_hours == 0U ? CL_TR("Latest") : CL_TR("Earlier"));
    cl_view_text(s,v->assets,CL_FONT_DETAIL,24,110,text,v->theme->muted,590);
    cl_view_footer(s,v,CL_TR("Left/Right Scroll   L/R Zoom   B Back"));
}

void cl_tools_draw(SDL_Surface *surface, const struct cl_view *v, const struct cl_tools *t)
{
    struct cl_tool_snapshot translated=t->snapshot;
    struct cl_tools localized=*t;
    const char *note=CL_TR(t->snapshot.status);
    cl_copy_utf8(translated.status,sizeof(translated.status),note);
    bool owned=t->snapshot.kind==CL_TOOL_CLOCK || t->snapshot.kind==CL_TOOL_FILE_TRANSFER ||
        t->snapshot.kind==CL_TOOL_SCRAPER || t->snapshot.kind==CL_TOOL_DOWNLOADS ||
        t->snapshot.kind==CL_TOOL_DEVICE || t->snapshot.kind==CL_TOOL_PACKAGES ||
        t->snapshot.kind==CL_TOOL_LAN_SERVICES;
    if(owned) for(size_t i=0U;i<translated.count;++i) {
        cl_copy_utf8(translated.rows[i].label,sizeof(translated.rows[i].label),CL_TR(t->snapshot.rows[i].label));
        if(strcmp(t->snapshot.rows[i].id,"@user")!=0 && strcmp(t->snapshot.rows[i].id,"@repository")!=0)
            cl_copy_utf8(translated.rows[i].detail,sizeof(translated.rows[i].detail),CL_TR(t->snapshot.rows[i].detail));
    }
    localized.snapshot=translated;t=&localized;
    static const char *const tabs[]={"VERIFIED","APPS","EXPERT"};
    const struct cl_theme *theme=v->theme;
    if (t->snapshot.kind == CL_TOOL_BATTERY) { battery_draw(surface,v,t); return; }
    size_t indices[CL_TOOL_ROWS],count=visible(t,indices);
    static const char *const icon_categories[] = {"Console icons", "App icons", "Expert icons"};
    if(t->snapshot.kind==CL_TOOL_GALLERY && count>0U && t->snapshot.ready && !t->waiting){
        char position[28];
        (void)snprintf(position,sizeof(position),"%zu / %zu",(size_t)t->snapshot.enabled_mask+t->selected+1U,
                       (size_t)t->snapshot.available_mask);
        cl_view_gallery(surface,v,t->snapshot.rows[t->selected].label,position,t->summary);
        return;
    }
    const char *title=t->snapshot.kind==CL_TOOL_LAN_SERVICES?"SMB / SFTP file sharing":
        t->snapshot.kind==CL_TOOL_FILES?"File Explorer":
        t->snapshot.kind==CL_TOOL_GALLERY?"Gallery":t->snapshot.kind==CL_TOOL_ICON_EDITOR ?
        (t->snapshot.enabled_mask == 1U && !t->snapshot.error ? t->snapshot.status :
         icon_categories[t->snapshot.available_mask < 3U ? t->snapshot.available_mask : 0U]) :
        t->snapshot.kind==CL_TOOL_ICON_PACKS?"Icon packs":t->snapshot.kind==CL_TOOL_MAINTENANCE?"Tools":t->snapshot.kind==CL_TOOL_CLOCK?"Date & Time":t->snapshot.kind==CL_TOOL_CORES?"Game core":t->snapshot.kind==CL_TOOL_FILE_TRANSFER?"File transfer":t->snapshot.kind==CL_TOOL_PACKAGES?"Package Manager":
        (t->snapshot.kind==CL_TOOL_ACTIVITY?"Activity Tracker":
         (t->snapshot.kind==CL_TOOL_NETWORK?"Wi-Fi":(t->snapshot.kind==CL_TOOL_DEVICE?"About device":
          (t->snapshot.kind==CL_TOOL_SCRAPER?"ScreenScraper":(t->snapshot.kind==CL_TOOL_DOWNLOADS?"Downloads":"Themes")))));
    cl_view_frame(surface,v,CL_TR(title));
    if(t->password_edit){
        char entered[32], detail[96];
        size_t length=strlen(t->password), shown=length>24U?24U:length;
        const char *action[5] = {t->keyboard_upper?"abc":"ABC",
            t->keyboard_page==0U?"Symbols":(t->keyboard_page==1U?"More":"Letters"),
            "Space", "Delete", t->snapshot.kind==CL_TOOL_NETWORK?"Join":(t->snapshot.kind==CL_TOOL_SCRAPER&&t->edit_field==8U?"Scrape":"Save")};
        const char *page = t->keyboard_page==0U?(t->keyboard_upper?"ABC uppercase":"abc lowercase"):
                           (t->keyboard_page==1U?"123 + symbols":"More symbols");
        if(t->password_visible)memcpy(entered,t->password+length-shown,shown);
        else memset(entered,'*',shown);
        entered[shown]='|';entered[shown+1U]='\0';
        cl_view_text(surface,v->assets,CL_FONT_LIST,24,74,t->snapshot.rows[t->network_row].label,theme->fonts[CL_FONT_LIST].color,592);
        cl_round_rect(surface,(SDL_Rect){24,110,592,44},6,theme->panel);
        cl_view_text(surface,v->assets,CL_FONT_LIST,36,119,entered,theme->accent,568);
        (void)snprintf(detail,sizeof(detail),"%s    %zu / 63 characters",CL_TR(page),length);
        /* Use the readable list font throughout entry. A decorative hint font
         * may fold letter case or clip the keyboard controls at 640 pixels. */
        cl_view_text(surface,v->assets,CL_FONT_LIST,24,164,detail,theme->muted,592);
        for(size_t i=0U;i<35U;++i){
            SDL_Rect key = i<30U?(SDL_Rect){24+(int)(i%10U)*59,198+(int)(i/10U)*47,55,41}:
                                    (SDL_Rect){24+(int)(i-30U)*120,345,112,43};
            bool active=i==t->keyboard_cursor;
            char letter[2]={keyboard_character(t,i),'\0'};
            const char *label=i<30U?letter:CL_TR(action[i-30U]);
            cl_round_rect(surface,key,6,active?theme->selected:theme->panel);
            cl_view_text(surface,v->assets,CL_FONT_LIST,key.x+(i<30U?18:8),key.y+7,label,
                         active?theme->selected_text:theme->fonts[CL_FONT_LIST].color,key.w-(i<30U?20:16));
        }
        cl_view_text(surface,v->assets,CL_FONT_LIST,24,402,
            t->keyboard_error[0]!='\0'?t->keyboard_error:CL_TR("A Type   X Delete   Y Show/Hide   B Cancel"),
            t->keyboard_error[0]!='\0'?theme->warning:theme->muted,592);
        cl_view_text(surface,v->assets,CL_FONT_LIST,24,442,
            t->snapshot.kind==CL_TOOL_NETWORK?CL_TR("L1/R1 Page  SELECT Case  START Join"):CL_TR("L1/R1 Page  SELECT Case  START Save"),
            theme->fonts[CL_FONT_HINT].color,592);return;
    }
    if(t->snapshot.kind==CL_TOOL_NETWORK && t->confirm_exit){
        cl_view_text(surface,v->assets,CL_FONT_LIST,24,140,CL_TR("Forget every saved Wi-Fi network?"),theme->warning,592);
        tool_hint(surface,v,24,440,CL_TR("A Forget   B Cancel"),theme->fonts[CL_FONT_HINT].color,592);return;
    }
    if(t->snapshot.kind==CL_TOOL_NETWORK)
        cl_view_text(surface,v->assets,CL_FONT_DETAIL,24,72,t->snapshot.status,theme->muted,592);
    if(t->snapshot.kind==CL_TOOL_SCRAPER || t->snapshot.kind==CL_TOOL_DOWNLOADS){
        if(t->summary){
            size_t used=0U,length=strlen(t->snapshot.status);int y=110;
            while(used<length&&y<370){char line[49];size_t n=length-used;if(n>44U)n=44U;
                if(used+n<length)while(n>1U&&t->snapshot.status[used+n]!=' ')--n;
                while(n>0U && ((unsigned char)t->snapshot.status[used+n]&0xc0U)==0x80U)--n;
                if(n==0U)break;
                memcpy(line,t->snapshot.status+used,n);line[n]='\0';
                cl_view_text(surface,v->assets,CL_FONT_LIST,24,y,line,theme->fonts[CL_FONT_LIST].color,592);
                used+=n;while(used<length&&t->snapshot.status[used]==' ')++used;y+=36;
            }
            tool_hint(surface,v,24,440,CL_TR("SELECT Return   B Back"),theme->fonts[CL_FONT_HINT].color,592);return;
        }
        cl_view_text(surface,v->assets,CL_FONT_DETAIL,24,72,CL_TR("SELECT Details / setup help"),theme->muted,592);
    }
    if(t->snapshot.kind==CL_TOOL_PACKAGES){
        unsigned int pending=0U;
        uint64_t changes=t->desired^t->snapshot.enabled_mask;
        char label[32];
        while(changes!=0U){changes&=changes-1U;++pending;}
        (void)snprintf(label,sizeof(label),CL_TR("%u pending"),pending);
        cl_view_text(surface,v->assets,CL_FONT_DETAIL,478,72,label,theme->muted,138);
        for(size_t i=0U;i<3U;++i)
            tool_hint(surface,v,24+(int)i*152,72,CL_TR(tabs[i]),i==(size_t)t->group?theme->accent:theme->muted,148);
    }
    if(t->waiting || t->snapshot.error || !t->snapshot.ready){
        const char *message=t->waiting?(t->last_operation==CL_TOOL_STOP_FILE_TRANSFER?"Stopping transfer and flushing the card...":t->committing?"Applying and flushing changes...":
            (t->snapshot.kind==CL_TOOL_SCRAPER||t->snapshot.kind==CL_TOOL_DOWNLOADS?"Working... B cancels transfer":"Loading...")):t->snapshot.status;
        cl_view_text(surface,v->assets,CL_FONT_LIST,28,140,message,theme->warning,584);
        if(t->waiting && t->snapshot.kind==CL_TOOL_SCRAPER && t->batch_scrape){
            char progress[160];uint64_t saved=t->snapshot.available_mask>>32U,skipped=t->snapshot.available_mask&UINT32_MAX;
            (void)snprintf(progress,sizeof(progress),CL_TR("Saved %llu, skipped %llu. Next position: %llu"),(unsigned long long)saved,(unsigned long long)skipped,(unsigned long long)(t->snapshot.revision+1U));
            cl_view_text(surface,v->assets,CL_FONT_DETAIL,28,210,progress,theme->fonts[CL_FONT_DETAIL].color,584);
            cl_view_text(surface,v->assets,CL_FONT_DETAIL,28,252,CL_TR("Checking ROM, then requesting artwork..."),theme->muted,584);
        }
        tool_hint(surface,v,28,438,t->snapshot.error?CL_TR("A Retry     B Back"):CL_TR("Please wait"),theme->muted,584);return;}
    if(t->snapshot.kind==CL_TOOL_PACKAGES && t->summary){
        unsigned int installs=0U,removals=0U;char line[96];
        for(size_t i=0U;i<t->snapshot.count;++i){uint64_t bit=UINT64_C(1)<<t->snapshot.rows[i].index;
            if((t->desired&bit)!=0U && (t->snapshot.enabled_mask&bit)==0U)++installs;
            if((t->desired&bit)==0U && (t->snapshot.enabled_mask&bit)!=0U)++removals;}
        (void)snprintf(line,sizeof(line),CL_TR("Enable: %u       Disable: %u"),installs,removals);
        cl_view_text(surface,v->assets,CL_FONT_LIST,28,132,line,theme->fonts[CL_FONT_LIST].color,584);
        cl_view_text(surface,v->assets,CL_FONT_DETAIL,28,190,CL_TR("Press START again to apply the pending changes."),theme->muted,584);
        cl_view_text(surface,v->assets,CL_FONT_DETAIL,28,232,CL_TR("ROMs, saves and bundled binaries are retained."),theme->muted,584);
        tool_hint(surface,v,28,438,CL_TR("START Apply     B Edit     Y Rollback"),theme->fonts[CL_FONT_HINT].color,584);return;}
    if(t->snapshot.kind==CL_TOOL_MAINTENANCE){
        cl_view_footer(surface,v,CL_TR("B Back"));
        return;
    }
    if(count==0U)cl_view_text(surface,v->assets,CL_FONT_LIST,28,148,CL_TR("No entries"),theme->muted,584);
    for(size_t visible_row=t->first;visible_row<count && visible_row<t->first+page_rows(t);++visible_row){
        const struct cl_tool_row *row=&t->snapshot.rows[indices[visible_row]];char label[160];
        bool activity=t->snapshot.kind==CL_TOOL_ACTIVITY || t->snapshot.kind==CL_TOOL_DEVICE;
        int y=104+(int)(visible_row-t->first)*(activity?76:43);bool selected=visible_row==t->selected;
        if(selected)cl_round_rect(surface,(SDL_Rect){20,y-3,600,activity?68:40},theme->radius,theme->selected);
        if(t->snapshot.kind==CL_TOOL_PACKAGES){uint64_t bit=UINT64_C(1)<<row->index;
            (void)snprintf(label,sizeof(label),"%s %s%s%s",(t->desired&bit)!=0U?"[x]":"[ ]",row->label,row->required?" *":"",((t->desired^t->snapshot.enabled_mask)&bit)!=0U?" +":"");}
        else if(t->snapshot.kind==CL_TOOL_CLOCK && visible_row<5U){
            time_t stamp=(time_t)t->desired;struct tm value;int field=0;
            if(localtime_r(&stamp,&value)!=NULL)field=visible_row==0U?value.tm_year+1900:visible_row==1U?value.tm_mon+1:visible_row==2U?value.tm_mday:visible_row==3U?value.tm_hour:value.tm_min;
            (void)snprintf(label,sizeof(label),"%s: %d",row->label,field);
        }else if(t->snapshot.kind==CL_TOOL_CLOCK && visible_row==7U)(void)snprintf(label,sizeof(label),CL_TR("Time zone: %s"),cl_timezones[t->timezone]);
        else (void)snprintf(label,sizeof(label),"%s%s",row->enabled?"* ":"",row->label);
        cl_view_text(surface,v->assets,CL_FONT_LIST,30,y,label,selected?theme->selected_text:(row->available?theme->fonts[CL_FONT_LIST].color:theme->muted),580);
        if(activity)cl_view_text(surface,v->assets,CL_FONT_DETAIL,30,y+32,row->detail,selected?theme->selected_text:theme->muted,580);
    }
    if(count>0U && t->snapshot.kind!=CL_TOOL_ACTIVITY && t->snapshot.kind!=CL_TOOL_DEVICE && t->snapshot.kind!=CL_TOOL_THEMES){const struct cl_tool_row *row=&t->snapshot.rows[indices[t->selected]];
        cl_view_text(surface,v->assets,CL_FONT_LIST,28,410,row->detail,theme->muted,584);}
    tool_hint(surface,v,28,448,t->snapshot.kind==CL_TOOL_LAN_SERVICES?CL_TR("A Toggle   X Refresh status   B Back"):
        t->snapshot.kind==CL_TOOL_FILES?CL_TR("A Open   L1/R1 Page   B Back"):
        t->snapshot.kind==CL_TOOL_GALLERY?CL_TR("MENU Exit   B Back"):
        t->snapshot.kind==CL_TOOL_PACKAGES?CL_TR("A Toggle  X Bulk  Y Reset  START Review"):
        (t->snapshot.kind==CL_TOOL_ACTIVITY?CL_TR("Y Raw paths   LEFT/RIGHT Page   B Back"):
         (t->snapshot.kind==CL_TOOL_NETWORK?CL_TR("A Select   X Refresh   B Back"):
          (t->snapshot.kind==CL_TOOL_DEVICE?CL_TR("Left/Right Page   B Back"):
           (t->snapshot.kind==CL_TOOL_CLOCK?CL_TR("LEFT/RIGHT Change   A Select   X Refresh   B Back"):t->snapshot.kind==CL_TOOL_FILE_TRANSFER?CL_TR("A Start/Stop   X Refresh address   B Back"):
            (t->snapshot.kind==CL_TOOL_CORES?CL_TR("A Set core   B Back"):
             (t->snapshot.kind==CL_TOOL_SCRAPER||t->snapshot.kind==CL_TOOL_DOWNLOADS?CL_TR("A Select  SELECT Help  B Back"):CL_TR("A Apply   SELECT Preview   B Back"))))))),theme->fonts[CL_FONT_HINT].color,584);
}
