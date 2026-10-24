#include "mc/encoding/xml.h"

#include "expat.h"

// CHUNK_BYTES is the most one XML_Parse call reads, since it takes an int length.
enum { CHUNK_BYTES = 1 << 30 };

// Parser turns Expat's callbacks into events.
typedef struct Parser {
    XML_Parser expat;
    Arena *arena;
    XmlEvents *events;
    // text gathers the characters Expat passes in pieces until the next tag.
    StringBuilder text;
    bool in_text;
    size_t text_line;
} Parser;

static size_t current_line(const Parser *p)
{
    return (size_t)XML_GetCurrentLineNumber64(p->expat);
}

static XmlEvent *push_event(Parser *p, XmlEventKind kind, size_t line)
{
    XmlEvents *events = p->events;
    events->items = arena_grow(p->arena, events->items, &events->capacity, events->count, sizeof(XmlEvent));
    XmlEvent *event = &events->items[events->count++];
    *event = (XmlEvent){ .kind = kind, .line = line };
    return event;
}

// end_text turns the characters gathered since the last tag into one event.
static void end_text(Parser *p)
{
    if (p->in_text) {
        p->in_text = false;
        push_event(p, XML_EVENT_TEXT, p->text_line)->text = str_builder_finish(&p->text);
    }
}

static void on_start(void *userdata, const XML_Char *name, const XML_Char **attributes)
{
    Parser *p = userdata;
    end_text(p);
    size_t count = 0;
    while (attributes[2 * count] != nullptr) {
        count++;
    }
    XmlEvent *event = push_event(p, XML_EVENT_START, current_line(p));
    event->name = str_copy(p->arena, S(name));
    event->attributes = arena_push(p->arena, arena_size_mul(count, sizeof(XmlAttribute)));
    event->attribute_count = count;
    for (size_t i = 0; i < count; i++) {
        event->attributes[i] = (XmlAttribute){
            .name = str_copy(p->arena, S(attributes[2 * i])),
            .value = str_copy(p->arena, S(attributes[2 * i + 1])),
        };
    }
}

static void on_end(void *userdata, const XML_Char *name)
{
    Parser *p = userdata;
    end_text(p);
    push_event(p, XML_EVENT_END, current_line(p))->name = str_copy(p->arena, S(name));
}

static void on_text(void *userdata, const XML_Char *text, int len)
{
    Parser *p = userdata;
    if (!p->in_text) {
        p->in_text = true;
        p->text = str_builder_create(p->arena, (size_t)len);
        p->text_line = current_line(p);
    }
    str_builder_append(&p->text, (String){ text, (size_t)len });
}

Error xml_parse(Arena *arena, String text, XmlEvents *events, Err *err)
{
    *events = (XmlEvents){ 0 };
    Parser p = { .expat = XML_ParserCreate(nullptr), .arena = arena, .events = events };
    if (p.expat == nullptr) {
        return err_set(err, ERR_OUT_OF_MEMORY, "xml: Expat could not create a parser");
    }
    XML_SetUserData(p.expat, &p);
    XML_SetElementHandler(p.expat, on_start, on_end);
    XML_SetCharacterDataHandler(p.expat, on_text);
    Error result = ERR_OK;
    size_t at = 0;
    do {
        size_t chunk = min_size(text.len - at, CHUNK_BYTES);
        at += chunk;
        if (XML_Parse(p.expat, text.data + at - chunk, (int)chunk, at == text.len) != XML_STATUS_OK) {
            result = err_set(err, ERR_PARSE, "xml: line %zu, column %zu: %s", current_line(&p),
                             (size_t)XML_GetCurrentColumnNumber64(p.expat) + 1,
                             XML_ErrorString(XML_GetErrorCode(p.expat)));
            break;
        }
    } while (at < text.len);
    XML_ParserFree(p.expat);
    return result;
}
