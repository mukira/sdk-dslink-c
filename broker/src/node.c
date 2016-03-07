#include <stdlib.h>
#include <string.h>
#include <jansson.h>
#include <dslink/mem/mem.h>
#include <dslink/utils.h>
#include "broker/stream.h"
#include "broker/node.h"
#include "broker/msg/msg_list.h"

BrokerNode *broker_node_get(BrokerNode *root,
                            const char *path, char **out) {
    if (!root) {
        return NULL;
    } else if (strcmp(path, "/") == 0) {
        return root;
    } else if (*path == '/') {
        path++;
    }

    BrokerNode *node = root;
    char *end = strchr(path, '/');
    if (end) {
        if (!node->children) {
            return NULL;
        }
        ref_t *ref = dslink_map_getl(node->children, (void *) path, end - path);
        if (!ref) {
            return NULL;
        }
        node = ref->data;
        if (node && node->type == DOWNSTREAM_NODE) {
            *out = end;
            return node;
        }
        return broker_node_get(node, end, out);
    } else if (*path != '\0') {
        if (!node->children) {
            return NULL;
        }
        ref_t *ref = dslink_map_get(node->children, (void *) path);
        if (!ref) {
            return NULL;
        }
        return ref->data;
    }

    return node;
}

BrokerNode *broker_node_create(const char *name, const char *profile) {
    size_t nameLen = strlen(name);
    size_t profileLen = strlen(profile);
    return broker_node_createl(name, nameLen, profile, profileLen);
}

BrokerNode *broker_node_createl(const char *name, size_t nameLen,
                                const char *profile, size_t profileLen) {
    if (!profile) {
        return NULL;
    }

    BrokerNode *node = dslink_calloc(1, sizeof(BrokerNode));
    if (!node) {
        return NULL;
    }

    node->type = REGULAR_NODE;
    node->name = dslink_strdupl(name, nameLen);
    if (!node->name) {
        dslink_free(node);
        return NULL;
    }

    node->children = dslink_malloc(sizeof(Map));
    if (dslink_map_init(node->children, dslink_map_str_cmp,
                        dslink_map_str_key_len_cal) != 0) {
        DSLINK_CHECKED_EXEC(free, node->children);
        dslink_free((void *) node->name);
        dslink_free(node);
        return NULL;
    }

    node->meta = json_object();
    if (!node->meta) {
        dslink_map_free(node->children);
        dslink_free((void *) node->name);
        dslink_free(node);
        return NULL;
    }
    listener_init(&node->on_value_update);
    listener_init(&node->on_child_added);
    listener_init(&node->on_child_removed);

    json_t *json = json_stringn(profile, profileLen);
    json_object_set_new_nocheck(node->meta, "$is", json);
    return node;
}

static
void broker_node_update_child(BrokerNode *parent, const char* name) {
    if (parent->list_stream) {
        update_list_child(parent, parent->list_stream, name);
    }

    ref_t *ref = dslink_map_get(parent->children, (void *) name);
    if (ref) {
        BrokerNode *child = ref->data;
        listener_dispatch_message(&parent->on_child_added, child);
    } else {
        listener_dispatch_message(&parent->on_child_removed, NULL);
    }
}

int broker_node_add(BrokerNode *parent, BrokerNode *child) {
    if (!(child && parent && parent->children)
        || dslink_map_contains(parent->children, (void *) child->name)) {
        return 1;
    }

    {
        size_t pathLen = strlen(parent->path);
        if (pathLen == 1 && *parent->path == '/') {
            pathLen = 0;
        }
        size_t nameLen = strlen(child->name);
        char *path = dslink_malloc(pathLen + nameLen + 2);
        child->path = path;
        if (!path) {
            return 1;
        }
        memcpy(path, parent->path, pathLen);
        *(path + pathLen) = '/';
        memcpy(path + pathLen + 1, child->name, nameLen + 1);
    }

    if (dslink_map_set(parent->children,
                       dslink_ref((void *) child->name, NULL),
                       dslink_ref(child, NULL)) != 0) {
        return 1;
    }
    child->parent = parent;
    broker_node_update_child(parent, child->name);

    return 0;
}

void broker_node_free(BrokerNode *node) {
    if (!node) {
        return;
    }

    if (node->children) {
        dslink_map_foreach_nonext(node->children) {
            dslink_decref(entry->key);
            {
                BrokerNode *child = entry->value->data;
                child->parent = NULL;
                broker_node_free(child);
                dslink_decref(entry->value);
            }
            MapEntry *tmp = entry->next;
            free(entry->node);
            free(entry);
            entry = tmp;
        }
        dslink_free(node->children->table);
        dslink_free(node->children);
    }

    if (node->type == DOWNSTREAM_NODE) {
        dslink_map_free(&((DownstreamNode *)node)->list_streams);
    } else {
        // TODO: add a new type for these listeners
        // they shouldn't be part of base node type
        listener_remove_all(&node->on_value_update);
        listener_remove_all(&node->on_child_added);
        listener_remove_all(&node->on_child_removed);
    }

    if (node->parent) {
        void *tmp = (void *) node->name;
        dslink_map_remove(node->parent->children, tmp);
    }

    json_decref(node->meta);
    dslink_free((void *) node->name);
    dslink_free((void *) node->path);
    dslink_free(node);
}

uint32_t broker_node_incr_rid(DownstreamNode *node) {
    if (node->rid > (UINT32_MAX - 1)) {
        // Loop it around
        node->rid = 0;
    }
    return ++node->rid;
}

uint32_t broker_node_incr_sid(DownstreamNode *node) {
    if (node->sid > (UINT32_MAX - 1)) {
        // Loop it around
        node->sid = 0;
    }
    return node->sid++;
}

void broker_node_update_value(BrokerNode *node, json_t *value, uint8_t isNewValue) {
    if (node->value) {
        json_decref(node->value);
    }
    node->value = value;
    if (!isNewValue) {
        json_incref(value);
    }
    listener_dispatch_message(&node->on_value_update, node);
}

void broker_dslink_disconnect(DownstreamNode *node) {
    dslink_map_foreach(&node->list_streams) {
        BrokerListStream *stream = entry->value->data;
        broker_stream_list_disconnect(stream);
    }

    dslink_map_foreach(&node->local_subs) {
        Listener *l = entry->value->data;
        listener_remove(l);
        dslink_free(l->data);
        dslink_free(l);
    }

    dslink_map_foreach(&node->sub_sids) {
        BrokerSubStream *stream = entry->value->data;
        dslink_map_remove(&stream->clients, node->link);
    }

    dslink_map_clear(&node->local_subs);
    dslink_map_clear(&node->sub_paths);
    dslink_map_clear(&node->sub_sids);

    node->link = NULL;
}

void broker_dslink_connect(DownstreamNode *node, RemoteDSLink *link) {
    node->link = link;
    dslink_map_foreach(&node->list_streams) {
        BrokerListStream *stream = entry->value->data;
        broker_stream_list_connect(stream, node);
    }
}
