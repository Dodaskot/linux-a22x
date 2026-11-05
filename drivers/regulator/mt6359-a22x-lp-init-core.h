/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT6359_A22X_LP_INIT_CORE_H
#define MT6359_A22X_LP_INIT_CORE_H

/*
 * A22x MT6359P experiment: the four LDO subsets of the vendor MT6833
 * SODI3 then Deepidle init calls, in their observed order. No VSEL writes.
 * The same transaction code is compiled by the host fault-injection harness.
 */
struct a22x_lp_field {
	unsigned int address, mask;
};

static const struct a22x_lp_field a22x_lp_init_fields[] = {
	{ 0x1b9c, 0x1400 }, { 0x1ba0, 0x0005 }, { 0x1ba6, 0x0005 },
	{ 0x1bc0, 0x1400 }, { 0x1bc4, 0x0005 }, { 0x1bca, 0x0005 },
	{ 0x1c42, 0x0800 }, { 0x1c46, 0x0002 }, { 0x1c4c, 0x0002 },
	{ 0x1c66, 0x1400 }, { 0x1c6a, 0x0005 }, { 0x1c70, 0x0005 },
};

struct a22x_lp_update {
	unsigned int field, mask;
};

/* Each update sets its one source-qualified bit. VA09 occurs in both phases. */
static const struct a22x_lp_update a22x_lp_init_updates[] = {
	{ 3, 0x0400 }, { 4, 0x0001 }, { 5, 0x0001 }, /* VRF12 SRCLKEN0 */
	{ 9, 0x0400 }, {10, 0x0001 }, {11, 0x0001 }, /* VA12  SRCLKEN0 */
	{ 6, 0x0800 }, { 7, 0x0002 }, { 8, 0x0002 }, /* VA09  SRCLKEN1 */
	{ 0, 0x0400 }, { 1, 0x0001 }, { 2, 0x0001 }, /* VXO22 SRCLKEN0 */
	{ 3, 0x1000 }, { 4, 0x0004 }, { 5, 0x0004 }, /* VRF12 SRCLKEN2 */
	{ 9, 0x1000 }, {10, 0x0004 }, {11, 0x0004 }, /* VA12  SRCLKEN2 */
	{ 6, 0x0800 }, { 7, 0x0002 }, { 8, 0x0002 }, /* VA09  SRCLKEN1 */
	{ 0, 0x1000 }, { 1, 0x0004 }, { 2, 0x0004 }, /* VXO22 SRCLKEN2 */
};

struct a22x_lp_io {
	int (*read)(void *ctx, unsigned int reg, unsigned int *value);
	int (*update)(void *ctx, unsigned int reg, unsigned int mask,
		      unsigned int value);
	u64 (*now)(void *ctx);
	void *ctx;
};

struct a22x_lp_step {
	u64 begin, end;
	unsigned int readback;
	int write_error, read_error;
};

struct a22x_lp_transaction {
	unsigned int pre[12], final[12], rollback[12];
	struct a22x_lp_step step[24], undo[24];
	unsigned int saved, attempted, verified, undo_attempted, undo_verified;
	int error, rollback_error;
	bool complete;
};

static int a22x_lp_verify(const struct a22x_lp_io *io, unsigned int *values)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(a22x_lp_init_fields); i++) {
		const struct a22x_lp_field *f = &a22x_lp_init_fields[i];

		ret = io->read(io->ctx, f->address, &values[i]);
		if (ret)
			return ret;
		if ((values[i] & f->mask) != f->mask)
			return -ESTALE;
	}
	return 0;
}

static int a22x_lp_apply(const struct a22x_lp_io *io,
			struct a22x_lp_transaction *t)
{
	unsigned int i;
	int ret;

	/* A transaction object cannot be replayed, even after a preimage failure. */
	if (t->error || t->saved || t->attempted || t->complete)
		return -EALREADY;
	for (i = 0; i < ARRAY_SIZE(a22x_lp_init_fields); i++) {
		ret = io->read(io->ctx, a22x_lp_init_fields[i].address, &t->pre[i]);
		if (ret) {
			t->error = ret;
			return ret;
		}
		t->saved++;
	}
	for (i = 0; i < ARRAY_SIZE(a22x_lp_init_updates); i++) {
		const struct a22x_lp_update *u = &a22x_lp_init_updates[i];
		const struct a22x_lp_field *f = &a22x_lp_init_fields[u->field];
		struct a22x_lp_step *s = &t->step[i];

		/* Include a failed write in rollback: the bus may have committed it. */
		t->attempted = i + 1;
		s->begin = io->now(io->ctx);
		s->read_error = -ECANCELED;
		s->write_error = io->update(io->ctx, f->address, u->mask, u->mask);
		ret = s->write_error;
		if (!ret) {
			s->read_error = io->read(io->ctx, f->address, &s->readback);
			ret = s->read_error;
			if (!ret && (s->readback & u->mask) != u->mask)
				ret = -EIO;
		}
		s->end = io->now(io->ctx);
		if (ret)
			goto rollback;
		t->verified++;
	}
	ret = a22x_lp_verify(io, t->final);
	if (ret)
		goto rollback;
	t->complete = true;
	return 0;

rollback:
	t->error = ret;
	/* Restore only attempted owned bits, in reverse source order. */
	for (i = t->attempted; i > 0; i--) {
		const struct a22x_lp_update *u = &a22x_lp_init_updates[i - 1];
		const struct a22x_lp_field *f = &a22x_lp_init_fields[u->field];
		struct a22x_lp_step *s = &t->undo[i - 1];
		unsigned int wanted = t->pre[u->field] & u->mask;

		t->undo_attempted++;
		s->begin = io->now(io->ctx);
		s->write_error = io->update(io->ctx, f->address, u->mask, wanted);
		/* Read even after write error; retain that error regardless of value. */
		s->read_error = io->read(io->ctx, f->address, &s->readback);
		s->end = io->now(io->ctx);
		ret = s->write_error ? s->write_error : s->read_error;
		if (!ret && (s->readback & u->mask) != wanted)
			ret = -EIO;
		if (ret && !t->rollback_error)
			t->rollback_error = ret;
		if (!ret)
			t->undo_verified++;
	}
	/* Independent final owned-bit check, including fields not yet written. */
	for (i = 0; i < ARRAY_SIZE(a22x_lp_init_fields); i++) {
		const struct a22x_lp_field *f = &a22x_lp_init_fields[i];

		ret = io->read(io->ctx, f->address, &t->rollback[i]);
		if (!ret && ((t->rollback[i] ^ t->pre[i]) & f->mask))
			ret = -ESTALE;
		if (ret && !t->rollback_error)
			t->rollback_error = ret;
	}
	return t->error;
}
#endif
