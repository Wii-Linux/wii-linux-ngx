//
// Created by ash on 9/01/24.
//

#ifndef __LATTE_IPC_H
#define __LATTE_IPC_H

void latteipc_init(void);
void latteipc_starbuck_msg(u32 msg);
void latteipc_ipi_cpu(int cpu);
void latteipc_setup_ipis(void);

#ifdef CONFIG_PPC_EARLY_DEBUG_LATTEIPC
void latteipc_init_early(void);
#endif

#endif //__LATTE_IPC_H
