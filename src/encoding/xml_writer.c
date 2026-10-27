#include "mc/encoding/xml.h"

XmlWriter xml_writer_create(Arena *arena)
{
    XmlWriter xml = { .out = str_builder_create(arena, 1024) };
    str_builder_append(&xml.out, S("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"));
    return xml;
}

void xml_writer_start(XmlWriter *xml, String name, const XmlAttribute *attributes, size_t attribute_count)
{
    str_builder_append_char(&xml->out, '<');
    str_builder_append(&xml->out, name);
    for (size_t i = 0; i < attribute_count; i++) {
        str_builder_append_char(&xml->out, ' ');
        str_builder_append(&xml->out, attributes[i].name);
        str_builder_append(&xml->out, S("=\""));
        xml_escape_into(&xml->out, attributes[i].value);
        str_builder_append_char(&xml->out, '"');
    }
    str_builder_append_char(&xml->out, '>');
}

void xml_writer_end(XmlWriter *xml, String name)
{
    str_builder_append(&xml->out, S("</"));
    str_builder_append(&xml->out, name);
    str_builder_append_char(&xml->out, '>');
}

void xml_writer_text(XmlWriter *xml, String text)
{
    xml_escape_into(&xml->out, text);
}

void xml_writer_element(XmlWriter *xml, String name, String text)
{
    xml_writer_start(xml, name, nullptr, 0);
    xml_writer_text(xml, text);
    xml_writer_end(xml, name);
}

String xml_writer_finish(XmlWriter *xml)
{
    return str_builder_finish(&xml->out);
}

void xml_escape_into(StringBuilder *out, String text)
{
    for (size_t i = 0; i < text.len; i++) {
        switch (text.data[i]) {
        case '&':
            str_builder_append(out, S("&amp;"));
            break;
        case '<':
            str_builder_append(out, S("&lt;"));
            break;
        case '>':
            str_builder_append(out, S("&gt;"));
            break;
        case '"':
            str_builder_append(out, S("&quot;"));
            break;
        case '\'':
            str_builder_append(out, S("&#39;"));
            break;
        default:
            str_builder_append_char(out, text.data[i]);
            break;
        }
    }
}
