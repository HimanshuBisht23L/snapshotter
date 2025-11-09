#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x0296695f, "refcount_warn_saturate" },
	{ 0x87a21cb3, "__ubsan_handle_out_of_bounds" },
	{ 0x6bc3fbc0, "__unregister_chrdev" },
	{ 0xdc50aae2, "__ref_stack_chk_guard" },
	{ 0x13c49cc2, "_copy_from_user" },
	{ 0x7c8e582e, "find_get_pid" },
	{ 0xec409e8e, "pid_task" },
	{ 0x6edd5941, "put_pid" },
	{ 0xa916b694, "strnlen" },
	{ 0x476b165a, "sized_strscpy" },
	{ 0x658a0cb9, "init_user_ns" },
	{ 0xb9bb4f23, "from_kuid" },
	{ 0x2cf56265, "__dynamic_pr_debug" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0x2a59e6a5, "__fortify_panic" },
	{ 0xbdfb6dbb, "__fentry__" },
	{ 0x5b8239ca, "__x86_return_thunk" },
	{ 0xf3bbbbd5, "__register_chrdev" },
	{ 0x92997ed8, "_printk" },
	{ 0x64b7e605, "__put_task_struct" },
	{ 0xa1a94982, "module_layout" },
};

MODULE_INFO(depends, "");

