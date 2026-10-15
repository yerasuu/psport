/* The XML reader for shadPS4/GoldHEN patch files (loader_patches.c): elements, attributes and the
 * text before an element's first child, with errors worded and placed as expat reports them. */
#ifndef LOADER_XML_H
#define LOADER_XML_H
#include <stddef.h>
/* Hidden, as main.h is: generic names kept out of the dynamic symbol table. */
#pragma GCC visibility push(hidden)

typedef struct { char *sz_name, *sz_value; } XmlAttribute;
typedef struct XmlNode {
    char *sz_name;
    XmlAttribute *p_attributes;
    size_t c_attributes;
    char *sz_text; /* the character data before the first child element */
    struct XmlNode **pp_children;
    size_t c_children;
} XmlNode;
typedef struct { char *p_data; size_t cb_length, cb_capacity; } Buffer;
typedef struct { const XmlNode **pp_nodes; size_t c_nodes, c_capacity; } NodeList;

void buffer_add(Buffer *p_buffer, const char *p_data, size_t cb_data);
char *buffer_take(Buffer *p_buffer);
int is_space(char c);
void free_node(XmlNode *p_node);
const char *attribute(const XmlNode *p_node, const char *sz_name);
void find_all(const XmlNode *p_node, const char *sz_name, NodeList *p_out);
void find_from(const XmlNode *p_root, const char *sz_name, NodeList *p_out);
XmlNode *read_xml(const char *sz_path, char *sz_out_error, size_t cb_error);
#pragma GCC visibility pop
#endif
