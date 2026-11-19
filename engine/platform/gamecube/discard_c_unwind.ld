/*
 * GameCube link: drop the DWARF unwind tables of the plain-C libraries.
 *
 * The engine's own C is already built with -fno-asynchronous-unwind-tables
 * (wscript), but the prebuilt libogc / newlib / libsysbase / libfat / iso9660
 * / asnd archives carry .eh_frame for every function: ~47 KiB of the
 * MEM1-resident image (libogc 28 KiB, libc/libm 16.5 KiB).  Unwind tables are
 * only read when a C++ exception propagates through a frame on its way to a
 * handler.  These libraries never throw, and no C++ in the image catches an
 * exception that could unwind through them (the only catch in the game code,
 * VGUI's TeamFortressViewport, is not linked), so dropping them changes no
 * behaviour.  libgcc keeps its tables: the unwinder reads its own.
 *
 * Passed to the linker as an implicit script: it augments the default
 * GameCube script, and its rule is placed before the .eh_frame output
 * section so these inputs are discarded first.
 */
SECTIONS
{
	/DISCARD/ :
	{
		*libogc.a:*(.eh_frame .eh_frame.*)
		*libc.a:*(.eh_frame .eh_frame.*)
		*libm.a:*(.eh_frame .eh_frame.*)
		*libsysbase.a:*(.eh_frame .eh_frame.*)
		*libfat.a:*(.eh_frame .eh_frame.*)
		*libiso9660.a:*(.eh_frame .eh_frame.*)
		*libasnd.a:*(.eh_frame .eh_frame.*)
	}
}
INSERT BEFORE .eh_frame;
