#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/sched.h>
#include <linux/mm_types_task.h>
#include<linux/syscalls.h>
#include<linux/mm.h>
#include<linux/pgtable.h>
#include <asm/pgtable_types.h>
#include<asm/tlbflush.h>
#include<linux/gfp.h>
#include<linux/uaccess.h>
#include<asm/page_64.h>
#include<linux/list.h>

/* check vma is stack memory or file memory*/

bool check_anon(struct vm_area_struct *vma)
{
    
    if(VM_STACK & vma->vm_flags)
        return false;
    if(vma->vm_file)
        return false;
    return true;
}

/*traverse page table and check physical memory exixt or not for vma region */
bool check_phy_page(unsigned long address)
{
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte;

    //check entry of pagetable

    pgd = pgd_offset(current->mm,address);

    if(!pgd_present(*pgd))
        return 0;

    p4d = p4d_offset(pgd, address);

    if(!p4d_present(*p4d))
        return 0;

    pud = pud_offset(p4d, address);

    if(!pud_present(*pud))
        return 0;

    pmd = pmd_offset(pud, address);

    if(!pmd_present(*pmd))
        return 0;

    pte = pte_offset_kernel(pmd, address);

    if(!pte_present(*pte))
        return 0;


    return 1;
}

void copy_pages(struct vm_area_struct *vma)
{
    struct process_context *new_context = NULL;
    struct page *new_page = NULL;
    void *page_addr = NULL;
    unsigned long n = 0;

    for(unsigned long i = vma->vm_start; i <= vma->vm_end; i+=4096)
    {  
        //if physical page present copy it to kernel memory
        if(check_phy_page(i))
        {
            new_context = kmalloc(sizeof(struct process_context),GFP_KERNEL);
            new_page = alloc_page(GFP_KERNEL); //get free pageframe in memory
            page_addr = page_address(new_page); //find virtual address
            n = copy_from_user(page_addr,(void*)i,4096); 
            if(n != 0)
            {
                //printk("error copying page \n");
            }
            new_context->new_page = new_page;
            new_context->address = i;
            list_add_tail(&new_context->context_list,&current->context_queue);
        }
    }
}

void save_context(void)
{
    struct mm_struct *mm = current->mm;
    struct vm_area_struct *mmap = mm->mmap;
    //initialize list head
    INIT_LIST_HEAD(&current->context_queue);
    while(mmap->vm_next)
    { 
        //if vma is anon copy it
        if(check_anon(mmap))
        {
            copy_pages(mmap);
        }
        mmap = mmap->vm_next;
    }

}

void restore_context(void)
{
    

}

SYSCALL_DEFINE1(mmcontext, int, i)
{
    if(i == 0)
        save_context();
    else
        restore_context();
    return 0;

}