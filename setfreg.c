#include <linux/module.h>  /* Needed by all modules */
#include <linux/kernel.h>  /* Needed for KERN_ALERT */
#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>




#define CPU_SPEED_ADDRESS 		0xC0888CE8
#define FREG_STAT_ADDRESS		0xC08D8340

#define NEW_MAX_FREQ			1804800
#define NEW_VDD_MV			1400


#define VREF_SEL     1	/* 0: 0.625V (50mV step), 1: 0.3125V (25mV step). */
#define V_STEP       (25 * (2 - VREF_SEL)) /* Minimum voltage step size. */
#define VREG_DATA    (VREG_CONFIG | (VREF_SEL << 5))
#define VREG_CONFIG  (BIT(7) | BIT(6)) /* Enable VREG, pull-down if disabled. */
/* Cause a compile error if the voltage is not a multiple of the step size. */
#define MV(mv)      ((mv) / (!((mv) % V_STEP)))
/* mv = (750mV + (raw * 25mV)) * (2 - VREF_SEL) */
#define VDD_RAW(mv) (((MV(mv) / V_STEP) - 30) | VREG_DATA)
#define BUF_SIZE 1024

enum acpuclk_source {
	LPXO	= -2,
	AXI	= -1,
	PLL_0	=  0,
	PLL_1,
	PLL_2,
	PLL_3,
	MAX_SOURCE
};

struct pll 
{
	unsigned int l;
	unsigned int m;
	unsigned int n;
	unsigned int pre_div;
};

struct clkctl_acpu_speed 
{
	unsigned int	use_for_scaling;
	unsigned int	acpu_clk_khz;
	int		src;
	unsigned int	acpu_src_sel;
	unsigned int	acpu_src_div;
	unsigned int	axi_clk_hz;
	unsigned int	vdd_mv;
	unsigned int	vdd_raw;
	struct pll	*pll_rate;
	unsigned long	lpj; /* loops_per_jiffy */
};


struct cpufreq_stats {
	unsigned int cpu;
	unsigned int total_trans;
	unsigned long long  last_time;
	unsigned int max_state;
	unsigned int state_num;
	unsigned int last_index;
	u64 	    *time_in_state;
	unsigned int *freq_table;
#ifdef CONFIG_CPU_FREQ_STAT_DETAILS
	unsigned int *trans_table;
#endif
};

struct clkctl_acpu_speed * lp_clock_speed;
struct cpufreq_stats * cpufreq_stats_table;
struct cpufreq_frequency_table *table;
struct cpufreq_policy * policy;
unsigned short last_acpu_freg;
unsigned long cur_index;

static int proc_read_rate(char *buffer, char **buffer_location, off_t offset, int count, int *eof, void *data)
{
        int ret = 0, i;
       
        if (offset <= 0)
		for(i = 0;lp_clock_speed[i].acpu_clk_khz;i++)
		        ret += scnprintf
			(
				&buffer[ret],
				count,
				"speed: index = [%i] %u %u %i %u %u %u %u %u %u\npll: %u %u %u %u\n",
				i,
				lp_clock_speed[i].use_for_scaling,
				lp_clock_speed[i].acpu_clk_khz,
				lp_clock_speed[i].src,
				lp_clock_speed[i].acpu_src_sel,
				lp_clock_speed[i].acpu_src_div,
				lp_clock_speed[i].axi_clk_hz,
				lp_clock_speed[i].vdd_mv,
				lp_clock_speed[i].vdd_raw,
				lp_clock_speed[i].lpj,
				(lp_clock_speed[i].pll_rate ? lp_clock_speed[i].pll_rate->l : 0),
				(lp_clock_speed[i].pll_rate ? lp_clock_speed[i].pll_rate->m : 0),
				(lp_clock_speed[i].pll_rate ? lp_clock_speed[i].pll_rate->n : 0),
				(lp_clock_speed[i].pll_rate ? lp_clock_speed[i].pll_rate->pre_div : 0)
			);
	

        return ret;
}

static int proc_read_index(char *buffer, char **buffer_location, off_t offset, int count, int *eof, void *data)
{    
        if (offset > 0)
                return 0;
        else
                return scnprintf(buffer, count, "%u", cur_index);
}

static int proc_write_index(struct file *filp, const char __user *buffer, unsigned long len, void *data)
{
	char buf[BUF_SIZE];

        if(!len || len >= BUF_SIZE)
                return -ENOSPC;
        if(copy_from_user(buf, buffer, len))
                return -EFAULT;
        buf[len] = 0;
        if(sscanf(buf, "%u", &cur_index))
	{
		if(cur_index > last_acpu_freg)
			cur_index = last_acpu_freg;
	}
        return len;

}

static int proc_read_scaling(char *buffer, char **buffer_location, off_t offset, int count, int *eof, void *data)
{    
        if (offset > 0)
                return 0;
        else
                return scnprintf(buffer, count, "%u", lp_clock_speed[cur_index].use_for_scaling);
}

static int proc_write_scaling(struct file *filp, const char __user *buffer, unsigned long len, void *data)
{
        int result;
	char buf[BUF_SIZE];

        if(!len || len >= BUF_SIZE)
                return -ENOSPC;
        if(copy_from_user(buf, buffer, len))
                return -EFAULT;
        buf[len] = 0;
        if(sscanf(buf, "%u",&lp_clock_speed[cur_index].use_for_scaling))
	{
		if(lp_clock_speed[cur_index].use_for_scaling > 1)
			lp_clock_speed[cur_index].use_for_scaling = 1;
	}
        return len;

}

static int proc_read_clk_khz(char *buffer, char **buffer_location, off_t offset, int count, int *eof, void *data)
{    
        if (offset > 0)
                return 0;
        else
                return scnprintf(buffer, count, "%u", lp_clock_speed[cur_index].acpu_clk_khz);
}

static int proc_write_clk_khz(struct file *filp, const char __user *buffer, unsigned long len, void *data)
{
        ulong newrate;
	unsigned int i;
	char buf[BUF_SIZE];

        if(!len || len >= BUF_SIZE)
                return -ENOSPC;
        if(copy_from_user(buf, buffer, len))
                return -EFAULT;
        buf[len] = 0;
        if(sscanf(buf, "%u", &newrate))
	{
		for (i = 0; table[i].frequency != CPUFREQ_TABLE_END; i++) 
		{
	        	if(table[i].frequency == CPUFREQ_ENTRY_INVALID)
	            		continue;
	        	if(table[i].frequency == lp_clock_speed[cur_index].acpu_clk_khz)
			{
				cpufreq_stats_table->freq_table[i] = table[i].frequency = newrate;
				break;
			}
		}
		if(cur_index == last_acpu_freg)
			policy->user_policy.max = policy->cpuinfo.max_freq = policy->max = newrate;
		
		lp_clock_speed[cur_index].acpu_clk_khz = newrate;
	}
        return len;

}

static int proc_read_src(char *buffer, char **buffer_location, off_t offset, int count, int *eof, void *data)
{    
        if (offset > 0)
                return 0;
        else
                return scnprintf(buffer, count, "%i", lp_clock_speed[cur_index].src);
}

static int proc_write_src(struct file *filp, const char __user *buffer, unsigned long len, void *data)
{
	char buf[BUF_SIZE];

        if(!len || len >= BUF_SIZE)
                return -ENOSPC;
        if(copy_from_user(buf, buffer, len))
                return -EFAULT;
        buf[len] = 0;
        if(sscanf(buf, "%i", &lp_clock_speed[cur_index].src))
	{
		if(lp_clock_speed[cur_index].src > MAX_SOURCE)
			lp_clock_speed[cur_index].src = MAX_SOURCE;
		else if(lp_clock_speed[cur_index].src < LPXO)
			lp_clock_speed[cur_index].src = LPXO;
	}
        return len;
}

static int proc_read_src_sel(char *buffer, char **buffer_location, off_t offset, int count, int *eof, void *data)
{    
        if (offset > 0)
                return 0;
        else
                return scnprintf(buffer, count, "%u", lp_clock_speed[cur_index].acpu_src_sel);
}

static int proc_write_src_sel(struct file *filp, const char __user *buffer, unsigned long len, void *data)
{
	char buf[BUF_SIZE];

        if(!len || len >= BUF_SIZE)
                return -ENOSPC;
        if(copy_from_user(buf, buffer, len))
                return -EFAULT;
        buf[len] = 0;
        sscanf(buf, "%u", &lp_clock_speed[cur_index].acpu_src_sel);
        return len;
}

static int proc_read_src_div(char *buffer, char **buffer_location, off_t offset, int count, int *eof, void *data)
{    
        if (offset > 0)
                return 0;
        else
                return scnprintf(buffer, count, "%u", lp_clock_speed[cur_index].acpu_src_div);
}

static int proc_write_src_div(struct file *filp, const char __user *buffer, unsigned long len, void *data)
{
	char buf[BUF_SIZE];

        if(!len || len >= BUF_SIZE)
                return -ENOSPC;
        if(copy_from_user(buf, buffer, len))
                return -EFAULT;
        buf[len] = 0;
        sscanf(buf, "%u", &lp_clock_speed[cur_index].acpu_src_div);
        return len;
}

static int proc_read_clk_hz(char *buffer, char **buffer_location, off_t offset, int count, int *eof, void *data)
{    
        if (offset > 0)
                return 0;
        else
                return scnprintf(buffer, count, "%u", lp_clock_speed[cur_index].axi_clk_hz);
}

static int proc_write_clk_hz(struct file *filp, const char __user *buffer, unsigned long len, void *data)
{
	char buf[BUF_SIZE];

        if(!len || len >= BUF_SIZE)
                return -ENOSPC;
        if(copy_from_user(buf, buffer, len))
                return -EFAULT;
        buf[len] = 0;
        sscanf(buf, "%u", &lp_clock_speed[cur_index].axi_clk_hz);
        return len;
}

static int proc_read_vdd_mv(char *buffer, char **buffer_location, off_t offset, int count, int *eof, void *data)
{    
        if (offset > 0)
                return 0;
        else
                return scnprintf(buffer, count, "%u", lp_clock_speed[cur_index].vdd_mv);
}

static int proc_write_vdd_mv(struct file *filp, const char __user *buffer, unsigned long len, void *data)
{
	char buf[BUF_SIZE];

        if(!len || len >= BUF_SIZE)
                return -ENOSPC;
        if(copy_from_user(buf, buffer, len))
                return -EFAULT;
        buf[len] = 0;
        sscanf(buf, "%u", &lp_clock_speed[cur_index].vdd_mv);
        return len;
}

static int proc_read_vdd_raw(char *buffer, char **buffer_location, off_t offset, int count, int *eof, void *data)
{    
        if (offset > 0)
                return 0;
        else
                return scnprintf(buffer, count, "%u", lp_clock_speed[cur_index].vdd_raw);
}

static int proc_write_vdd_raw(struct file *filp, const char __user *buffer, unsigned long len, void *data)
{
        ulong newrate;
	char buf[BUF_SIZE];

        if(!len || len >= BUF_SIZE)
                return -ENOSPC;
        if(copy_from_user(buf, buffer, len))
                return -EFAULT;
        buf[len] = 0;
        sscanf(buf, "%u", &lp_clock_speed[cur_index].vdd_raw);
        return len;
}

static int proc_read_lpj(char *buffer, char **buffer_location, off_t offset, int count, int *eof, void *data)
{    
        if (offset > 0)
                return 0;
        else
                return scnprintf(buffer, count, "%u", lp_clock_speed[cur_index].lpj);
}

static int proc_write_lpj(struct file *filp, const char __user *buffer, unsigned long len, void *data)
{
	char buf[BUF_SIZE];

        if(!len || len >= BUF_SIZE)
                return -ENOSPC;
        if(copy_from_user(buf, buffer, len))
                return -EFAULT;
        buf[len] = 0;
        sscanf(buf, "%u", &lp_clock_speed[cur_index].lpj);
        return len;
}

static int proc_read_pll(char *buffer, char **buffer_location, off_t offset, int count, int *eof, void *data)
{    
        if (offset > 0)
                return 0;
        else{
		if(lp_clock_speed[cur_index].pll_rate)
		        return scnprintf
			(
				buffer,
	 			count,
	 			"%u %u %u %u",
	 			lp_clock_speed[cur_index].pll_rate->l,
				lp_clock_speed[cur_index].pll_rate->m,
				lp_clock_speed[cur_index].pll_rate->n,
				lp_clock_speed[cur_index].pll_rate->pre_div
			);
		else
			return scnprintf(buffer,count,"0 0 0 0");
	}
}

static int proc_write_pll(struct file *filp, const char __user *buffer, unsigned long len, void *data)
{
	char buf[BUF_SIZE];
        if(!len || len >= BUF_SIZE)
                return -ENOSPC;
        if(copy_from_user(buf, buffer, len))
                return -EFAULT;
	if(lp_clock_speed[cur_index].pll_rate)
	{
		buf[len] = 0;
	
		sscanf(buf, "%u %u %u %u", &lp_clock_speed[cur_index].pll_rate->l, &lp_clock_speed[cur_index].pll_rate->m,&lp_clock_speed[cur_index].pll_rate->n,&lp_clock_speed[cur_index].pll_rate->pre_div);
	}
        return len;
}

static int __init init_this_module(void)
{
 	struct proc_dir_entry *proc_entry;
	unsigned int i;
	lp_clock_speed = (struct clkctl_acpu_speed *)CPU_SPEED_ADDRESS;
	cpufreq_stats_table = *(struct cpufreq_stats **)FREG_STAT_ADDRESS;


	proc_mkdir("overclock", NULL);
	create_proc_read_entry("overclock/info_rate", 0444, NULL, proc_read_rate, NULL);
	create_proc_read_entry("overclock/cur_index", 0644, NULL, proc_read_index, NULL)->write_proc = proc_write_index;
	create_proc_read_entry("overclock/use_for_scaling", 0644, NULL, proc_read_scaling, NULL)->write_proc = proc_write_scaling;
	create_proc_read_entry("overclock/clk_khz", 0644, NULL, proc_read_clk_khz, NULL)->write_proc = proc_write_clk_khz;
	create_proc_read_entry("overclock/src", 0644, NULL, proc_read_src, NULL)->write_proc = proc_write_src;
	create_proc_read_entry("overclock/src_sel", 0644, NULL, proc_read_src_sel, NULL)->write_proc = proc_write_src_sel;
	create_proc_read_entry("overclock/src_div", 0644, NULL, proc_read_src_div, NULL)->write_proc = proc_write_src_div;
	create_proc_read_entry("overclock/clk_hz", 0644, NULL, proc_read_clk_hz, NULL)->write_proc = proc_write_clk_hz;
	create_proc_read_entry("overclock/vdd_mv", 0644, NULL, proc_read_vdd_mv, NULL)->write_proc = proc_write_vdd_mv;
	create_proc_read_entry("overclock/vdd_raw", 0644, NULL, proc_read_vdd_raw, NULL)->write_proc = proc_write_vdd_raw;
	create_proc_read_entry("overclock/pll", 0644, NULL, proc_read_pll, NULL)->write_proc = proc_write_pll;
	create_proc_read_entry("overclock/lpj", 0644, NULL, proc_read_lpj, NULL)->write_proc = proc_write_lpj;

   	printk("<1>Starting setfreg\n");

	//Output freg struct for test
	for(last_acpu_freg = 0;lp_clock_speed[last_acpu_freg].acpu_clk_khz != 0;last_acpu_freg++)
		printk
		(
			"clkctl_acpu_speed table for [%u]:\nuse_for_scaling=%u, acpu_clk_khz=%u, src=%u, acpu_src_sel=%u, acpu_src_div=%u, axi_clk_hz=%u, vdd_mv=%u, vdd_raw=%u, pll_rate=%x, lpj=%u\n",
			last_acpu_freg,
			lp_clock_speed[last_acpu_freg].use_for_scaling,
			lp_clock_speed[last_acpu_freg].acpu_clk_khz,
			lp_clock_speed[last_acpu_freg].src,
			lp_clock_speed[last_acpu_freg].acpu_src_sel,
			lp_clock_speed[last_acpu_freg].acpu_src_div,
			lp_clock_speed[last_acpu_freg].axi_clk_hz,
			lp_clock_speed[last_acpu_freg].vdd_mv,
			lp_clock_speed[last_acpu_freg].vdd_raw,
			lp_clock_speed[last_acpu_freg].pll_rate,
			lp_clock_speed[last_acpu_freg].lpj
		);
	
	 cur_index = --last_acpu_freg;
	
	//Output pll struct for test
	printk("pll table:\n l=%u, m=%u, n=%u, pre_div=%u\n", lp_clock_speed[11].pll_rate->l, lp_clock_speed[11].pll_rate->m, lp_clock_speed[11].pll_rate->n, lp_clock_speed[11].pll_rate->pre_div);

	table = cpufreq_frequency_get_table(0);
	printk("CPU freg table: ");
	//Output freg table
	for (i = 0; table[i].frequency != CPUFREQ_TABLE_END; i++) 
	{
	        if (table[i].frequency == CPUFREQ_ENTRY_INVALID)
	            continue;
	        printk("[%u]=%d ", i, table[i].frequency);
	}
	
	

	policy = cpufreq_cpu_get(0);
	printk("\nCurrent policy: cpuinfo.max_freg=%u, user max freg = %u, max=%u\n",policy->cpuinfo.max_freq,policy->user_policy.max,policy->max);
	
	printk("Stat test: [0]=%u\n",cpufreq_stats_table->freq_table[0]);
	/*
	for (i = 0; table[i].frequency != CPUFREQ_TABLE_END; i++) 
	{
	        if (table[i].frequency == CPUFREQ_ENTRY_INVALID)
	            continue;
	        if(table[i].frequency == lp_clock_speed[last_acpu_freg].acpu_clk_khz)
		{
			table[i].frequency = NEW_MAX_FREQ;
			cpufreq_stats_table->freq_table[i] = NEW_MAX_FREQ;
			break;
		}
	}
	
	lp_clock_speed[last_acpu_freg].acpu_clk_khz = NEW_MAX_FREQ;
	lp_clock_speed[last_acpu_freg].vdd_mv = NEW_VDD_MV;
	lp_clock_speed[last_acpu_freg].vdd_raw = VDD_RAW(NEW_VDD_MV);
	lp_clock_speed[last_acpu_freg].lpj = 5865344;

	//lp_clock_speed[11].lpj = cpufreq_scale(loops_per_jiffy, base_clk->acpu_clk_khz, acpu_freq_tbl[i].acpu_clk_khz);

	lp_clock_speed[last_acpu_freg].pll_rate->l = 94;
	lp_clock_speed[last_acpu_freg].pll_rate->m = 0;
	lp_clock_speed[last_acpu_freg].pll_rate->n = 1;
	lp_clock_speed[last_acpu_freg].pll_rate->pre_div = 0;
	


	 
	policy->user_policy.max = policy->cpuinfo.max_freq = policy->max = NEW_MAX_FREQ;
    */

	printk("<1>Success setfreg!\n");

	//return success
	return 0;
}


static void __exit exit_this_module(void)
{
  	printk(KERN_ALERT "Setfreg unloaded!\n");

	remove_proc_entry("overclock/info_rate", NULL);
	remove_proc_entry("overclock/cur_index", NULL);
	remove_proc_entry("overclock/use_for_scaling", NULL);
	remove_proc_entry("overclock/clk_khz", NULL);
	remove_proc_entry("overclock/src", NULL);
	remove_proc_entry("overclock/src_sel", NULL);
	remove_proc_entry("overclock/src_div", NULL);
	remove_proc_entry("overclock/clk_hz", NULL);
	remove_proc_entry("overclock/vdd_mv", NULL);
	remove_proc_entry("overclock/vdd_raw", NULL);
	remove_proc_entry("overclock/pll", NULL);
	remove_proc_entry("overclock/lpj", NULL);

	remove_proc_entry("overclock", NULL);
}

module_init(init_this_module);
module_exit(exit_this_module);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Andrew Solodov");
MODULE_DESCRIPTION("Set freguency cpu.");
MODULE_VERSION("1.0");
