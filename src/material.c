#include "material.h"
#include "texture.h"
#include "cache.h"
#include "collection.h"
#include "vmfparser.h"
#include "common.h"

struct MaterialContext {
	struct Material *mat;
	struct TokenContext tok;
	const char *key;
	int key_length;
	char value[128];
};

enum KeyValueResult {KeyValue_Read, KeyValue_Error, KeyValue_End};
static enum KeyValueResult getNextKeyValue(struct MaterialContext *ctx) {
	for (;;) {
		enum TokenType type = getNextToken(&ctx->tok);
		if (type == Token_End || type == Token_CurlyClose) return KeyValue_End;
		if (type != Token_String) return KeyValue_Error;

		ctx->key = ctx->tok.str_start;
		ctx->key_length = ctx->tok.str_length;

		type = getNextToken(&ctx->tok);
		if (type == Token_CurlyOpen) {
			/* skip unsupported proxies and DX-level specifics */
			PRINTF("Skipping section %.*s", ctx->key_length, ctx->key);

			int depth = 1;
			for (;;) {
				type = getNextToken(&ctx->tok);
				if (type == Token_CurlyClose) {
					if (--depth == 0)
						break;
				} else if (type == Token_CurlyOpen) {
					++depth;
				} else if (type != Token_String)
					return KeyValue_Error;
			}
		} else if (type != Token_String) {
			return KeyValue_Error;
		} else {
			if (ctx->tok.str_length > (int)sizeof(ctx->value) - 1) {
				PRINTF("Value is too long: %d", ctx->tok.str_length);
				return KeyValue_Error;
			}
			memcpy(ctx->value, ctx->tok.str_start, ctx->tok.str_length);
			ctx->value[ctx->tok.str_length] = '\0';
			return KeyValue_Read;
		}
	} /* loop until key value pair found */
} /* getNextKeyValue */

static const char * const ignore_params[] = {
	"$surfaceprop", "$surfaceprop2", "$tooltexture",
	"%tooltexture", "%keywords", "%compilewater", "%detailtype",
	"%compilenolight", "%compilepassbullets",
	0
};

static int materialLoad(struct IFile *file, struct ICollection *coll, struct Material *output, struct Stack *tmp) {
	char buffer[8192]; /* most vmts are < 300, a few are almost 1000, max seen ~3200 */
	if (file->size > sizeof(buffer) - 1) {
		PRINTF("VMT is too large: %zu", file->size);
		return 0;
	}

	if (file->size != file->read(file, 0, file->size, buffer)) return 0;

	buffer[file->size] = '\0';

	struct MaterialContext ctx;
	ctx.mat = output;
	ctx.tok.cursor = buffer;
	ctx.tok.end = NULL;

#define EXPECT_TOKEN(type) \
	if (getNextToken(&ctx.tok) != type) { \
		PRINTF("Unexpected token at position %zd, expecting %d; left: %s", ctx.tok.cursor - buffer, type, ctx.tok.cursor); \
		return 0; \
	}

	EXPECT_TOKEN(Token_String);
	const char *shader = ctx.tok.str_start;
	const int shader_length = ctx.tok.str_length;

	EXPECT_TOKEN(Token_CurlyOpen);

	for (;;) {
		const enum KeyValueResult result = getNextKeyValue(&ctx);
		if (result == KeyValue_End) break;
		if (result != KeyValue_Read) goto error;

		int skip = 0;
		for (const char *const *ignore = ignore_params; ignore[0] != 0; ++ignore)
			if (strncasecmp(ignore[0], ctx.key, ctx.key_length) == 0) {
				skip = 1;
				break;
			}
		if (skip) continue;

		if (strncasecmp("$basetexture", ctx.key, ctx.key_length) == 0) {
			output->base_texture[0] = textureGet(ctx.value, coll, tmp);
		} else if (strncasecmp("$basetexture2", ctx.key, ctx.key_length) == 0) {
			output->base_texture[1] = textureGet(ctx.value, coll, tmp);
		} else if (strncasecmp("$basetexturetransform", ctx.key, ctx.key_length) == 0) {
		} else if (strncasecmp("$basetexturetransform2", ctx.key, ctx.key_length) == 0) {
		} else if (strncasecmp("$detail", ctx.key, ctx.key_length) == 0) {
			output->detail = textureGet(ctx.value, coll, tmp);
		} else if (strncasecmp("$detailscale", ctx.key, ctx.key_length) == 0) {
		} else if (strncasecmp("$detailblendfactor", ctx.key, ctx.key_length) == 0) {
		} else if (strncasecmp("$detailblendmode", ctx.key, ctx.key_length) == 0) {
		} else if (strncasecmp("$parallaxmap", ctx.key, ctx.key_length) == 0) {
		} else if (strncasecmp("$parallaxmapscale", ctx.key, ctx.key_length) == 0) {
		} else if (strncasecmp("$bumpmap", ctx.key, ctx.key_length) == 0) {
			output->bump = textureGet(ctx.value, coll, tmp);
		} else if (strncasecmp("$envmap", ctx.key, ctx.key_length) == 0) {
			output->envmap = textureGet(ctx.value, coll, tmp);
		} else if (strncasecmp("$fogenable", ctx.key, ctx.key_length) == 0) {
		} else if (strncasecmp("$fogcolor", ctx.key, ctx.key_length) == 0) {
		} else if (strncasecmp("$alphatest", ctx.key, ctx.key_length) == 0) {
		} else if (strncasecmp("$translucent", ctx.key, ctx.key_length) == 0) {
		} else if (strncasecmp("include", ctx.key, ctx.key_length) == 0) {
			char *vmt = strstr(ctx.value, ".vmt");
			if (vmt)
				*vmt = '\0';
			if (strstr(ctx.value, "materials/") == ctx.value)
				*output = *materialGet(ctx.value + 10, coll, tmp);
		} else {
			PRINTF("Material shader:%.*s, unknown param %.*s = %s",
					shader_length, shader, ctx.key_length, ctx.key, ctx.value);
		}
	} /* for all properties */

	if (!output->base_texture[0])
		output->base_texture[0] = cacheGetTexture("opensource/placeholder");

	return 1;

error:
	PRINTF("Error parsing material with shader %.*s: %s", shader_length, shader, ctx.tok.cursor);
	return 0;
}

const struct Material *materialGet(const char *name, struct ICollection *collection, struct Stack *tmp) {
	const struct Material *mat = cacheGetMaterial(name);
	if (mat) return mat;

	struct IFile *matfile;
	if (CollectionOpen_Success != collectionChainOpen(collection, name, File_Material, &matfile)) {
		PRINTF("Material \"%s\" not found", name);
		return cacheGetMaterial("opensource/placeholder");
	}

	struct Material localmat;
	memset(&localmat, 0, sizeof localmat);
	if (materialLoad(matfile, collection, &localmat, tmp) == 0) {
		PRINTF("Material \"%s\" found, but could not be loaded", name);
	} else {
		cachePutMaterial(name, &localmat);
		mat = cacheGetMaterial(name);
	}

	matfile->close(matfile);
	return mat ? mat : cacheGetMaterial("opensource/placeholder");
}
