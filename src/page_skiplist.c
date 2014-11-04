#include <string.h>
#include <stdlib.h>
#include "page_skiplist.h"
#include "page_multi_string.h"
#include "util.h"

static int rl_skiplist_random_level()
{
	int level = 1;
	while ((rand() & 0xFFFF) < (RL_SKIPLIST_PROBABILITY * 0xFFFF)) {
		level++;
	}
	return (level < RL_SKIPLIST_MAXLEVEL) ? level : RL_SKIPLIST_MAXLEVEL;
}

int rl_skiplist_create(rlite *db, rl_skiplist **_skiplist)
{
	rl_skiplist *skiplist = malloc(sizeof(rl_skiplist));
	if (skiplist == NULL) {
		return RL_OUT_OF_MEMORY;
	}
	skiplist->left = db->next_empty_page;
	skiplist->right = 0;
	skiplist->size = 0;
	skiplist->level = 1;

	rl_skiplist_node *node;
	int retval = rl_skiplist_node_create(db, &node, RL_SKIPLIST_MAXLEVEL, 0.0, 0);
	if (retval != RL_OK) {
		goto cleanup;
	}

	long j;
	for (j = 0; j < RL_SKIPLIST_MAXLEVEL; j++) {
		node->level[j].right = 0;
		node->level[j].span = 0;
	}

	retval = rl_write(db, &rl_data_type_skiplist_node, db->next_empty_page, node);
	if (retval != RL_OK) {
		goto cleanup;
	}

	*_skiplist = skiplist;
cleanup:
	return retval;
}

int rl_skiplist_destroy(rlite *db, void *skiplist)
{
	db = db;
	free(skiplist);
	return RL_OK;
}

int rl_skiplist_node_create(rlite *db, rl_skiplist_node **_node, long level, double score, long value)
{
	db = db;
	rl_skiplist_node *node = malloc(sizeof(rl_skiplist_node) + level * sizeof(struct rl_skiplist_node_level));
	if (!node) {
		return RL_OUT_OF_MEMORY;
	}
	node->num_levels = level;
	node->score = score;
	node->value = value;
	node->left = 0;
	long i;
	for (i = 0; i < level; i++) {
		node->level[i].right = 0;
		node->level[i].span = 0;
	}
	*_node = node;
	return RL_OK;
}

int rl_skiplist_node_destroy(rlite *db, void *node)
{
	db = db;
	free(node);
	return RL_OK;
}

int rl_skiplist_iterator_create(rlite *db, rl_skiplist_iterator **_iterator, rl_skiplist *skiplist, long next_node_page, int direction, int size)
{
	rl_skiplist_iterator *iterator = malloc(sizeof(rl_skiplist_iterator));
	if (!iterator) {
		return RL_OUT_OF_MEMORY;
	}
	iterator->db = db;
	iterator->skiplist = skiplist;
	if (direction == -1) {
		iterator->direction = -1;
	}
	else {
		iterator->direction = 1;
	}
	if (next_node_page) {
		iterator->node_page = next_node_page;
	}
	else {
		if (iterator->direction == 1) {
			iterator->node_page = skiplist->left;
			iterator->size = 1;
			iterator->position = 0;
			// first node_page is the index node, skip it
			int retval = rl_skiplist_iterator_next(iterator, NULL);
			if (retval != RL_OK) {
				free(iterator);
				return retval;
			}
		}
		else {
			iterator->node_page = skiplist->right;
		}
	}
	if (size) {
		iterator->size = size;
	}
	else {
		iterator->size = skiplist->size;
	}
	iterator->position = 0;
	*_iterator = iterator;
	return RL_OK;
}

int rl_skiplist_iterator_destroy(rlite *db, rl_skiplist_iterator *iterator)
{
	db = db;
	free(iterator);
	return RL_OK;
}
int rl_skiplist_iterator_next(rl_skiplist_iterator *iterator, rl_skiplist_node **retnode)
{
	if (!iterator->node_page || iterator->position == iterator->size) {
		return RL_END;
	}
	void *_node;
	rl_skiplist_node *node;
	int retval = rl_read(iterator->db, &rl_data_type_skiplist_node, iterator->node_page, iterator->skiplist, &_node, 1);
	if (retval != RL_FOUND) {
		return retval;
	}
	node = _node;
	if (iterator->direction == 1) {
		iterator->node_page = node->level[0].right;
	}
	else {
		iterator->node_page = node->left;
	}
	if (retnode) {
		*retnode = node;
	}
	iterator->position++;
	return RL_OK;
}

static int rl_skiplist_get_update(rlite *db, rl_skiplist *skiplist, double score, unsigned char *value, long valuelen, rl_skiplist_node *update_node[RL_SKIPLIST_MAXLEVEL], long update_node_page[RL_SKIPLIST_MAXLEVEL], long rank[RL_SKIPLIST_MAXLEVEL])
{
	void *_node;
	rl_skiplist_node *node, *next_node;
	long i, node_page, tmp;
	if (update_node_page) {
		for (i = 0; i < RL_SKIPLIST_MAXLEVEL; i++) {
			update_node_page[i] = 0;
		}
	}

	node_page = skiplist->left;
	int cmp, retval = rl_read(db, &rl_data_type_skiplist_node, skiplist->left, skiplist, &_node, 1);
	if (retval != RL_FOUND) {
		goto cleanup;
	}
	if (update_node_page) {
		update_node_page[0] = skiplist->left;
	}
	node_page = skiplist->left;
	node = _node;

	for (i = skiplist->level - 1; i >= 0; i--) {
		if (rank) {
			rank[i] = i == (skiplist->level - 1) ? 0 : rank[i + 1];
		}
		while (1) {
			tmp = node->level[i].right;
			if (tmp == 0) {
				break;
			}
			retval = rl_read(db, &rl_data_type_skiplist_node, tmp, skiplist, &_node, 1);
			if (retval != RL_FOUND) {
				goto cleanup;
			}
			next_node = _node;
			if (next_node->score > score) {
				break;
			}
			if (next_node->score == score) {
				if (value == NULL) {
					break;
				}
				retval = rl_multi_string_cmp_str(db, next_node->value, value, valuelen, &cmp);
				if (retval != RL_OK) {
					goto cleanup;
				}
				if (cmp >= 0) {
					break;
				}
			}
			if (rank) {
				rank[i] += node->level[i].span;
			}
			node = next_node;
			node_page = tmp;
		}
		update_node[i] = node;
		if (update_node_page) {
			update_node_page[i] = node_page;
		}
	}
	retval = RL_OK;
cleanup:
	return retval;
}

int rl_skiplist_add(rlite *db, rl_skiplist *skiplist, double score, unsigned char *value, long valuelen)
{
	void *_node;
	rl_skiplist_node *node;
	long node_page, tmp;
	long rank[RL_SKIPLIST_MAXLEVEL];
	rl_skiplist_node *update_node[RL_SKIPLIST_MAXLEVEL];
	long update_node_page[RL_SKIPLIST_MAXLEVEL];
	long value_page;
	int retval = rl_multi_string_set(db, &value_page, value, valuelen);
	if (retval != RL_OK) {
		goto cleanup;
	}
	retval = rl_skiplist_get_update(db, skiplist, score, value, valuelen, update_node, update_node_page, rank);
	if (retval != RL_OK) {
		goto cleanup;
	}

	long i, level = rl_skiplist_random_level();
	if (level > skiplist->level) {
		for (i = skiplist->level; i < level; i++) {
			rank[i] = 0;
			retval = rl_read(db, &rl_data_type_skiplist_node, skiplist->left, skiplist, &_node, 1);
			if (retval != RL_FOUND) {
				goto cleanup;
			}
			update_node[i] = _node;
			update_node[i]->level[i].span = skiplist->size;
			update_node_page[i] = skiplist->left;
		}
		skiplist->level = level;
	}
	retval = rl_skiplist_node_create(db, &node, level, score, value_page);
	if (retval != RL_OK) {
		goto cleanup;
	}
	node_page = db->next_empty_page;
	retval = rl_write(db, &rl_data_type_skiplist_node, db->next_empty_page, node);
	if (retval != RL_OK) {
		goto cleanup;
	}
	for (i = 0; i < level || i < skiplist->level; i++) {
		if (i < level) {
			node->level[i].right = update_node[i]->level[i].right;
			update_node[i]->level[i].right = node_page;

			node->level[i].span = update_node[i]->level[i].span - (rank[0] - rank[i]);
			update_node[i]->level[i].span = (rank[0] - rank[i]) + 1;
		}
		retval = rl_write(db, &rl_data_type_skiplist_node, update_node_page[i], update_node[i]);
		if (retval != RL_OK) {
			goto cleanup;
		}
	}

	for (i = level; i < skiplist->level; i++) {
		update_node[i]->level[i].span++;
	}

	node->left = (update_node_page[0] == skiplist->left) ? 0 : update_node_page[0];
	if (node->level[0].right) {
		tmp = node_page;
		node_page = node->level[0].right;
		retval = rl_read(db, &rl_data_type_skiplist_node, node_page, skiplist, &_node, 1);
		if (retval != RL_FOUND) {
			goto cleanup;
		}
		node = _node;
		node->left = tmp;
		retval = rl_write(db, &rl_data_type_skiplist_node, node_page, node);
		if (retval != RL_OK) {
			goto cleanup;
		}
	}
	else {
		skiplist->right = node_page;
	}
	skiplist->size++;
	retval = RL_OK;
cleanup:
	return retval;
}

int rl_skiplist_first_node(rlite *db, rl_skiplist *skiplist, double score, unsigned char *value, long valuelen, rl_skiplist_node **retnode, long *_rank)
{
	rl_skiplist_node *update_node[RL_SKIPLIST_MAXLEVEL];
	long rank[RL_SKIPLIST_MAXLEVEL];
	int retval = rl_skiplist_get_update(db, skiplist, score, value, valuelen, update_node, NULL, rank);
	if (retval != RL_OK) {
		goto cleanup;
	}
	if (update_node[0]->level[0].right) {
		if (retnode) {
			retval = rl_read(db, &rl_data_type_skiplist_node, update_node[0]->level[0].right, skiplist, (void **)retnode, 1);
		}
		if (_rank) {
			*_rank = rank[0];
		}
		retval = RL_FOUND;
	}
	else {
		retval = RL_NOT_FOUND;
	}
cleanup:
	return retval;

}

int rl_skiplist_delete(rlite *db, rl_skiplist *skiplist, double score, unsigned char *value, long valuelen)
{
	rl_skiplist_node *update_node[RL_SKIPLIST_MAXLEVEL];
	long update_node_page[RL_SKIPLIST_MAXLEVEL];
	int retval = rl_skiplist_get_update(db, skiplist, score, value, valuelen, update_node, update_node_page, NULL);
	if (retval != RL_OK) {
		goto cleanup;
	}

	void *_node;
	rl_skiplist_node *node, *next_node;
	long node_page = update_node[0]->level[0].right;
	retval = rl_read(db, &rl_data_type_skiplist_node, node_page, skiplist, &_node, 1);
	if (retval != RL_FOUND) {
		goto cleanup;
	}
	node = _node;

	int cmp;
	retval = rl_multi_string_cmp_str(db, node->value, value, valuelen, &cmp);
	if (retval != RL_OK) {
		goto cleanup;
	}

	if (node->score != score || cmp != 0) {
		retval = RL_NOT_FOUND;
		goto cleanup;
	}

	long i;
	for (i = 0; i < skiplist->level; i++) {
		if (update_node[i]->level[i].right == node_page) {
			update_node[i]->level[i].span += node->level[i].span - 1;
			update_node[i]->level[i].right = node->level[i].right;
		}
		else {
			update_node[i]->level[i].span--;
		}
		retval = rl_write(db, &rl_data_type_skiplist_node, update_node_page[i], update_node[i]);
		if (retval != RL_OK) {
			goto cleanup;
		}
	}

	long next_node_page = node->level[0].right;
	if (next_node_page) {
		retval = rl_read(db, &rl_data_type_skiplist_node, next_node_page, skiplist, &_node, 1);
		if (retval != RL_FOUND) {
			goto cleanup;
		}
		next_node = _node;
		next_node->left = node->left;
		retval = rl_write(db, &rl_data_type_skiplist_node, next_node_page, next_node);
		if (retval != RL_OK) {
			goto cleanup;
		}
	}
	else {
		skiplist->right = node->left;
	}

	retval = rl_skiplist_node_destroy(db, node);
	if (retval != RL_OK) {
		goto cleanup;
	}
	retval = rl_delete(db, node_page);
	if (retval != RL_OK) {
		goto cleanup;
	}

	retval = rl_read(db, &rl_data_type_skiplist_node, skiplist->left, skiplist, &_node, 1);
	if (retval != RL_FOUND) {
		goto cleanup;
	}
	node = _node;
	while (skiplist->level > 0 && node->level[skiplist->level - 1].right == 0) {
		skiplist->level--;
	}
	skiplist->size--;
	retval = RL_OK;
cleanup:
	return retval;
}

#ifdef DEBUG
int rl_skiplist_print(rlite *db, rl_skiplist *skiplist)
{
	printf("left: %ld, right: %ld, size: %ld, level: %ld\n", skiplist->left, skiplist->right, skiplist->size, skiplist->level);
	long page = skiplist->left;
	int retval;
	long i, j;
	void *_node;
	rl_skiplist_node *node;

	j = 0;
	while (page > 0) {
		if (j++ > skiplist->size + 1) {
			fprintf(stderr, "Too many nodes, expected %ld\n", skiplist->size);
			return RL_UNEXPECTED;
		}
		retval = rl_read(db, &rl_data_type_skiplist_node, page, skiplist, &_node, 1);
		if (retval != RL_FOUND) {
			goto cleanup;
		}
		node = _node;
		printf("page: %ld, value: %ld, score: %lf, left: %ld", page, node->value, node->score, node->left);
		for (i = 0; i < node->num_levels; i++) {
			if (node->level[i].right) {
				printf(", level[%ld].right: %ld, level[%ld].span: %ld", i, node->level[i].right, i, node->level[i].span);
			}
		}
		printf("\n");
		page = node->level[0].right;
	}
	retval = RL_OK;
cleanup:
	return retval;
}
#endif

int rl_skiplist_is_balanced(rlite *db, rl_skiplist *skiplist)
{
	long page = skiplist->left;
	int retval;
	long i = 0;
	void *_node;
	rl_skiplist_node *node;
	rl_skiplist_node **nodes = malloc(sizeof(rl_skiplist_node *) * (skiplist->size + 1));
	long *nodes_page = malloc(sizeof(long) * (skiplist->size + 1));

	while (page > 0) {
		if (i > skiplist->size + 1) {
			fprintf(stderr, "Too many nodes, expected %ld\n", skiplist->size);
			return RL_UNEXPECTED;
		}
		nodes_page[i] = page;
		retval = rl_read(db, &rl_data_type_skiplist_node, page, skiplist, &_node, 1);
		if (retval != RL_FOUND) {
			goto cleanup;
		}
		node = _node;
		nodes[i++] = node;
		page = node->level[0].right;
	}
	if (i != skiplist->size + 1) {
		fprintf(stderr, "Number of nodes (%ld) doesn't match skiplist size (%ld)\n", i - 1, skiplist->size);
		retval = RL_UNEXPECTED;
		goto cleanup;
	}
	int cmp;

	// node 0 is not really a value node, ignore it
	for (i = 2; i < skiplist->size; i++) {
		if (nodes[i - 1]->score > nodes[i]->score) {
			fprintf(stderr, "skiplist score is not sorted at position %ld (%lf > %lf)\n", i, nodes[i - 1]->score, nodes[i]->score);
			retval = RL_UNEXPECTED;
			goto cleanup;
		}
		else if (nodes[i - 1]->score == nodes[i]->score) {
			retval = rl_multi_string_cmp(db, nodes[i - 1]->value, nodes[i]->value, &cmp);
			if (retval != RL_OK) {
				goto cleanup;
			}
			if (cmp > 0) {
				fprintf(stderr, "skiplist score is not sorted at position %ld (same score, cmp is %d)\n", i, cmp);
				retval = RL_UNEXPECTED;
				goto cleanup;
			}
		}
	}

	for (i = 2; i < skiplist->size; i++) {
		if (nodes[i]->left != nodes_page[i - 1]) {
			fprintf(stderr, "expected skiplist node[%ld] left page to be %ld, got %ld\n", i, nodes_page[i - 1], nodes[i]->left);
			retval = RL_UNEXPECTED;
			goto cleanup;
		}
	}

	long pos;
	for (i = 1; i < skiplist->level; i++) {
		node = nodes[0];
		if (node->level[i].right == 0) {
			fprintf(stderr, "There has to be at least one node in level %ld\n", i);
			retval = RL_UNEXPECTED;
			goto cleanup;
		}
		page = 1;
		pos = 0;
		while (1) {
			pos += node->level[i].span;
			page = node->level[i].right;
			if (page == 0) {
				break;
			}
			if (nodes_page[pos] != page) {
				fprintf(stderr, "expected skiplist at level %ld node page to be %ld, got %ld\n", i, nodes_page[pos], page);
				retval = RL_UNEXPECTED;
				goto cleanup;
			}
			node = nodes[pos];
		}
	}

	retval = RL_OK;
cleanup:
	free(nodes_page);
	free(nodes);
	return retval;
}

int rl_skiplist_serialize(struct rlite *db, void *obj, unsigned char *data)
{
	db = db;
	rl_skiplist *skiplist = obj;
	put_4bytes(&data[0], skiplist->left);
	put_4bytes(&data[4], skiplist->right);
	put_4bytes(&data[8], skiplist->size);
	put_4bytes(&data[12], skiplist->level);
	return RL_OK;
}
int rl_skiplist_deserialize(struct rlite *db, void **obj, void *context, unsigned char *data)
{
	db = db;
	context = context;
	rl_skiplist *skiplist;
	int retval = rl_skiplist_create(db, &skiplist);
	if (retval != RL_OK) {
		goto cleanup;
	}
	skiplist->left = get_4bytes(&data[0]);
	skiplist->right = get_4bytes(&data[4]);
	skiplist->size = get_4bytes(&data[8]);
	skiplist->level = get_4bytes(&data[12]);
	*obj = skiplist;
	retval = RL_OK;
cleanup:
	return retval;
}

int rl_skiplist_node_serialize(struct rlite *db, void *obj, unsigned char *data)
{
	db = db;
	rl_skiplist_node *node = obj;
	put_4bytes(data, node->value);
	put_double(&data[4], node->score);
	put_4bytes(&data[12], node->left);
	put_4bytes(&data[16], node->num_levels);
	long i, pos = 20;
	for (i = 0; i < node->num_levels; i++) {
		put_4bytes(&data[pos], node->level[i].right);
		put_4bytes(&data[pos + 4], node->level[i].span);
		pos += 8;
	}
	return RL_OK;
}

int rl_skiplist_node_deserialize(struct rlite *db, void **obj, void *context, unsigned char *data)
{
	db = db;
	context = context;
	rl_skiplist_node *node;
	long value = get_4bytes(data);
	double score = get_double(&data[4]);
	long left = get_4bytes(&data[12]);
	long level = get_4bytes(&data[16]);;
	int retval = rl_skiplist_node_create(db, &node, level, score, value);
	if (retval != RL_OK) {
		goto cleanup;
	}
	node->left = left;
	long i, pos = 20;
	for (i = 0; i < node->num_levels; i++) {
		node->level[i].right = get_4bytes(&data[pos]);
		node->level[i].span = get_4bytes(&data[pos + 4]);
		pos += 8;
	}
	*obj = node;
cleanup:
	return retval;
}

int rl_skiplist_node_by_rank(rlite *db, rl_skiplist *skiplist, long rank, rl_skiplist_node **retnode)
{
	void *_node;
	rl_skiplist_node *node;
	long i, pos = 0;

	// increment the rank in 1 to ignore the first node thats not actually a data node
	rank++;

	int retval = rl_read(db, &rl_data_type_skiplist_node, skiplist->left, skiplist, &_node, 1);
	if (retval != RL_FOUND) {
		goto cleanup;
	}
	node = _node;

	for (i = skiplist->level - 1; i >= 0; i--) {
		if (pos == rank) {
			break;
		}
		while (1) {
			if (node->level[i].right == 0 || pos + node->level[i].span > rank) {
				break;
			}
			pos += node->level[i].span;
			retval = rl_read(db, &rl_data_type_skiplist_node, node->level[i].right, skiplist, &_node, 1);
			if (retval != RL_FOUND) {
				goto cleanup;
			}
			node = _node;
		}
	}
	if (rank != pos) {
		retval = RL_NOT_FOUND;
	}
	else {
		*retnode = node;
		retval = RL_OK;
	}
cleanup:
	return retval;
}
