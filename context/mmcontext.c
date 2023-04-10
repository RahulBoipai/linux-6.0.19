#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/sched.h>
#include <linux/mm_types_task.h>
#include<linux/syscalls.h>
#include<linux/mm.h>
#include <linux/errno.h>
#include<linux/pgtable.h>
#include <asm/pgtable_types.h>
#include<asm/tlbflush.h>
#include<linux/gfp.h>
#include<linux/uaccess.h>
#include<asm/page_64.h>
#include<linux/list.h>

/* check vma if not stack memory or file memory*/
bool check_anon(struct vm_area_struct *vma)
{
    
    if(VM_STACK & vma->vm_flags)
        return false;
    if(vma->vm_file)
        return false;
    return true;
}

/*traverse page table and check physical memory exist or not for vma region */
pte_t * check_page_pte(unsigned long address)
{
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte;

    //check entry in pagetable
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
        return NULL;

    return pte;
}

/*Make pages write protect to do copy on write*/
int set_protect(struct vm_area_struct *vma)
{
    pte_t * pte = NULL;
    //printk("protect pages");
    for(unsigned long i = vma->vm_start; i <= vma->vm_end; i+=4096) //starting address of page
    {  
        //if physical page present make it write protect
        pte =  check_page_pte(i);
        if(pte)
        {
            set_pte_at(current->mm , i,pte , pte_wrprotect(*pte));
            
        }
        else return -EINVAL;
    }
    return 0;
}

/*As page are made write protect this function copy page only when write page faults for pages, i.e copy on write*/
int copy_pages(struct page *page)
{
    struct process_context *new_context = NULL;
    struct page *new_page = NULL;
    void *page_addr = NULL;
    unsigned long n = 0;
    void *user_paddr = NULL;
    //create space in kernel memory
    new_context = kmalloc(sizeof(struct process_context),GFP_KERNEL);
    new_page = alloc_page(GFP_KERNEL); //get free pageframe in memory
    page_addr = page_address(new_page);
    user_paddr = page_address(page);
    //copy from user space to kernel space
    n = __copy_from_user((void*)page_addr,(void*)user_paddr,4096); 
    if(n != 0) return -EINVAL;

    new_context->new_page = new_page; //paged to which context saved
    new_context->address = (unsigned long)user_paddr; //user page address which context saved
    list_add_tail(&new_context->context_list,&current->context_queue);
    return 0;
}

/*Make memory region write protect and make it ready for saving if there is a write page faults */
void save_context(void)
{
    struct mm_struct *mm = current->mm;
    struct vm_area_struct *mmap = mm->mmap;
    //initialize list head
    INIT_LIST_HEAD(&current->context_queue);
    current->contextsave = true;
    while(mmap->vm_next)
    { 
        //if vma is anon copy it
        if(check_anon(mmap))
        {
            //copy_pages(mmap);
            set_protect(mmap);
        }
        mmap = mmap->vm_next;
    }

}

/*restore context and memory region to previous state before system call*/

int restore_context(void)
{
    
    struct list_head *context = &current->context_queue;
    struct process_context *next = NULL;
    void* new_addr = NULL;
    unsigned long n = 0;
    current->contextsave = false;
    
    while(context->next != context)
    {
        next = list_entry(context->next,struct process_context,context_list);
        list_del(context->next);
        new_addr = page_address(next->new_page);
        //copy from kernel to user space
        n = __copy_to_user((void*)next->address,new_addr,4096);
        if(n!=0) return -EINVAL;
        //free kernel pages
        __free_pages(next->new_page,0);
        kfree(next);
    }
    return 0;
}

/*clear saved context if task exit without restoring to previous state */
void clear_context(void)
{
    struct list_head *context = &current->context_queue;
    struct process_context *next = NULL;
    void* new_addr = NULL;
    current->contextsave = false;
    
    while(context->next != context)
    {
        next = list_entry(context->next,struct process_context,context_list);
        list_del(context->next);
        new_addr = page_address(next->new_page);
        __free_pages(next->new_page,0);
        kfree(next);
    }

}

SYSCALL_DEFINE1(mmcontext, int, i)
{
    if(i == 0){
       if(current->contextsave) 
       {
        return -EINVAL;
       }
       else save_context();
        return 0;
    }
    else if(i == 1)
    {
        if(!current->contextsave) 
       {
        return -EINVAL;
       }
        else restore_context();
        return 0;
    }
    return -EINVAL;

}