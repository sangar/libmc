#pragma once

#include "mc/core/error.h"
#include "mc/text/str.h"

typedef enum XmlEventKind {
    XML_EVENT_START,
    XML_EVENT_END,
    XML_EVENT_TEXT,
} XmlEventKind;

// XmlAttribute is one attribute of a start tag, its value with references
// decoded and whitespace normalized as XML 1.0 requires.
typedef struct XmlAttribute {
    String name;
    String value;
} XmlAttribute;

// XmlEvent is a start tag, an end tag or the text between two tags. Names are
// as written, prefix included, such as dc:creator; namespaces are not
// resolved. Text has its references decoded and CDATA sections unwrapped, and
// comes as one event however Expat split it, whitespace included.
typedef struct XmlEvent {
    XmlEventKind kind;
    // name is the element's, for XML_EVENT_START and XML_EVENT_END.
    String name;
    // text is the characters of XML_EVENT_TEXT.
    String text;
    // attributes are those of XML_EVENT_START in the order written, then the
    // defaults the DTD declares.
    XmlAttribute *attributes;
    size_t attribute_count;
    // line is the 1-based line where the event starts, for messages.
    size_t line;
} XmlEvent;

typedef struct XmlEvents {
    XmlEvent *items;
    size_t count;
    size_t capacity;
} XmlEvents;

// xml_parse reads a whole XML 1.0 document into events in arena. The text may
// be UTF-8, UTF-16 with a byte order mark, ISO-8859-1 or US-ASCII, as its
// declaration says; the events are UTF-8. Comments, processing instructions
// and the DTD are not events. Entities the DTD declares are expanded within
// Expat's limits on amplification, and external entities are never loaded. A
// document that is not well-formed, or expands past those limits, is
// ERR_PARSE with the line and column in err.
[[nodiscard]] Error xml_parse(Arena *arena, String text, XmlEvents *events, Err *err);
