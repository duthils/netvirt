/*
 * Generated by asn1c-0.9.23 (http://lionet.info/asn1c)
 * From ASN.1 module "DNDS"
 * 	found in "dnds.asn1"
 */

#include "AuthRequest.h"

static int check_permitted_alphabet_2(const void *sptr) {
	/* The underlying type is IA5String */
	const IA5String_t *st = (const IA5String_t *)sptr;
	const uint8_t *ch = st->buf;
	const uint8_t *end = ch + st->size;
	
	for(; ch < end; ch++) {
		uint8_t cv = *ch;
		if(!(cv <= 127)) return -1;
	}
	return 0;
}

static int
memb_certName_constraint_1(asn_TYPE_descriptor_t *td, const void *sptr,
			asn_app_constraint_failed_f *ctfailcb, void *app_key) {
	const IA5String_t *st = (const IA5String_t *)sptr;
	size_t size;
	
	if(!sptr) {
		_ASN_CTFAIL(app_key, td, sptr,
			"%s: value not given (%s:%d)",
			td->name, __FILE__, __LINE__);
		return -1;
	}
	
	size = st->size;
	
	if((size >= 1 && size <= 256)
		 && !check_permitted_alphabet_2(st)) {
		/* Constraint check succeeded */
		return 0;
	} else {
		_ASN_CTFAIL(app_key, td, sptr,
			"%s: constraint failed (%s:%d)",
			td->name, __FILE__, __LINE__);
		return -1;
	}
}

static asn_TYPE_member_t asn_MBR_AuthRequest_1[] = {
	{ ATF_NOFLAGS, 0, offsetof(struct AuthRequest, certName),
		(ASN_TAG_CLASS_CONTEXT | (0 << 2)),
		-1,	/* IMPLICIT tag at current level */
		&asn_DEF_IA5String,
		memb_certName_constraint_1,
		0,	/* PER is not compiled, use -gen-PER */
		0,
		"certName"
		},
};
static ber_tlv_tag_t asn_DEF_AuthRequest_tags_1[] = {
	(ASN_TAG_CLASS_UNIVERSAL | (16 << 2))
};
static asn_TYPE_tag2member_t asn_MAP_AuthRequest_tag2el_1[] = {
    { (ASN_TAG_CLASS_CONTEXT | (0 << 2)), 0, 0, 0 } /* certName at 85 */
};
static asn_SEQUENCE_specifics_t asn_SPC_AuthRequest_specs_1 = {
	sizeof(struct AuthRequest),
	offsetof(struct AuthRequest, _asn_ctx),
	asn_MAP_AuthRequest_tag2el_1,
	1,	/* Count of tags in the map */
	0, 0, 0,	/* Optional elements (not needed) */
	0,	/* Start extensions */
	2	/* Stop extensions */
};
asn_TYPE_descriptor_t asn_DEF_AuthRequest = {
	"AuthRequest",
	"AuthRequest",
	SEQUENCE_free,
	SEQUENCE_print,
	SEQUENCE_constraint,
	SEQUENCE_decode_ber,
	SEQUENCE_encode_der,
	SEQUENCE_decode_xer,
	SEQUENCE_encode_xer,
	0, 0,	/* No PER support, use "-gen-PER" to enable */
	0,	/* Use generic outmost tag fetcher */
	asn_DEF_AuthRequest_tags_1,
	sizeof(asn_DEF_AuthRequest_tags_1)
		/sizeof(asn_DEF_AuthRequest_tags_1[0]), /* 1 */
	asn_DEF_AuthRequest_tags_1,	/* Same as above */
	sizeof(asn_DEF_AuthRequest_tags_1)
		/sizeof(asn_DEF_AuthRequest_tags_1[0]), /* 1 */
	0,	/* No PER visible constraints */
	asn_MBR_AuthRequest_1,
	1,	/* Elements count */
	&asn_SPC_AuthRequest_specs_1	/* Additional specs */
};

