
/* Compiler implementation of the D programming language
 * Copyright (c) 1999-2016 by Digital Mars
 * All Rights Reserved
 * written by Walter Bright
 * http://www.digitalmars.com
 * Distributed under the Boost Software License, Version 1.0.
 * http://www.boost.org/LICENSE_1_0.txt
 * https://github.com/dlang/dmd/blob/master/src/toobj.c
 */

#include <stdio.h>
#include <stddef.h>
#include <time.h>
#include <assert.h>

#include "root.h"
#include "stringtable.h"
#include "frontend.h"

#include "cc.h"
#include "global.h"
#include "oper.h"
#include "code.h"
#include "type.h"
#include "dt.h"
#include "cgcv.h"
#include "outbuf.h"
#include "irstate.h"

extern bool obj_includelib(const char *name);
void obj_startaddress(Symbol *s);
void obj_lzext(Symbol *s1,Symbol *s2);
void obj_append(Dsymbol *s);

void TypeInfo_toDt(DtBuilder& dtb, TypeInfoDeclaration *d);
void Initializer_toDt(Initializer *init, DtBuilder& dtb);
void Type_toDt(Type *t, DtBuilder& dtb);
void ClassDeclaration_toDt(ClassDeclaration *cd, DtBuilder& dtb);
void StructDeclaration_toDt(StructDeclaration *sd, DtBuilder& dtb);
Symbol *toSymbol(Dsymbol *s);
void Expression_toDt(Expression *e, DtBuilder& dtb);
void FuncDeclaration_toObjFile(FuncDeclaration *fd, bool multiobj);
Symbol *toThunkSymbol(FuncDeclaration *fd, int offset);
Symbol *toVtblSymbol(ClassDeclaration *cd);
Symbol *toInitializer(AggregateDeclaration *ad);
Symbol *toInitializer(EnumDeclaration *ed);
void genTypeInfo(Type *t, Scope *sc);
bool isSpeculativeType(Type *t);

void toDebug(EnumDeclaration *ed);
void toDebug(StructDeclaration *sd);
void toDebug(ClassDeclaration *cd);

void objc_Module_genmoduleinfo_classes();

/* ================================================================== */

// Put out instance of ModuleInfo for this Module

void genModuleInfo(Module *m)
{
    //printf("Module::genmoduleinfo() %s\n", m->toChars());

    if (!Module::moduleinfo)
    {
        ObjectNotFound(Id::ModuleInfo);
    }

    Symbol *msym = toSymbol(m);

    //////////////////////////////////////////////

    m->csym->Sclass = SCglobal;
    m->csym->Sfl = FLdata;

    DtBuilder dtb;
    ClassDeclarations aclasses;

    //printf("members->dim = %d\n", members->dim);
    for (size_t i = 0; i < m->members->dim; i++)
    {
        Dsymbol *member = (*m->members)[i];

        //printf("\tmember '%s'\n", member->toChars());
        member->addLocalClass(&aclasses);
    }

    // importedModules[]
    size_t aimports_dim = m->aimports.dim;
    for (size_t i = 0; i < m->aimports.dim; i++)
    {
        Module *mod = m->aimports[i];
        if (!mod->needmoduleinfo)
            aimports_dim--;
    }

    FuncDeclaration *sgetmembers = m->findGetMembers();

    // These must match the values in druntime/src/object_.d
    #define MIstandalone      0x4
    #define MItlsctor         0x8
    #define MItlsdtor         0x10
    #define MIctor            0x20
    #define MIdtor            0x40
    #define MIxgetMembers     0x80
    #define MIictor           0x100
    #define MIunitTest        0x200
    #define MIimportedModules 0x400
    #define MIlocalClasses    0x800
    #define MIname            0x1000

    unsigned flags = 0;
    if (!m->needmoduleinfo)
        flags |= MIstandalone;
    if (m->sctor)
        flags |= MItlsctor;
    if (m->sdtor)
        flags |= MItlsdtor;
    if (m->ssharedctor)
        flags |= MIctor;
    if (m->sshareddtor)
        flags |= MIdtor;
    if (sgetmembers)
        flags |= MIxgetMembers;
    if (m->sictor)
        flags |= MIictor;
    if (m->stest)
        flags |= MIunitTest;
    if (aimports_dim)
        flags |= MIimportedModules;
    if (aclasses.dim)
        flags |= MIlocalClasses;
    flags |= MIname;

    dtb.dword(flags);        // _flags
    dtb.dword(0);            // _index

    if (flags & MItlsctor)
        dtb.xoff(m->sctor, 0, TYnptr);
    if (flags & MItlsdtor)
        dtb.xoff(m->sdtor, 0, TYnptr);
    if (flags & MIctor)
        dtb.xoff(m->ssharedctor, 0, TYnptr);
    if (flags & MIdtor)
        dtb.xoff(m->sshareddtor, 0, TYnptr);
    if (flags & MIxgetMembers)
        dtb.xoff(toSymbol(sgetmembers), 0, TYnptr);
    if (flags & MIictor)
        dtb.xoff(m->sictor, 0, TYnptr);
    if (flags & MIunitTest)
        dtb.xoff(m->stest, 0, TYnptr);
    if (flags & MIimportedModules)
    {
        dtb.size(aimports_dim);
        for (size_t i = 0; i < m->aimports.dim; i++)
        {
            Module *mod = m->aimports[i];

            if (!mod->needmoduleinfo)
                continue;

            Symbol *s = toSymbol(mod);

            /* Weak references don't pull objects in from the library,
             * they resolve to 0 if not pulled in by something else.
             * Don't pull in a module just because it was imported.
             */
            s->Sflags |= SFLweak;
            dtb.xoff(s, 0, TYnptr);
        }
    }
    if (flags & MIlocalClasses)
    {
        dtb.size(aclasses.dim);
        for (size_t i = 0; i < aclasses.dim; i++)
        {
            ClassDeclaration *cd = aclasses[i];
            dtb.xoff(toSymbol(cd), 0, TYnptr);
        }
    }
    if (flags & MIname)
    {
        // Put out module name as a 0-terminated string, to save bytes
        m->nameoffset = dtb.length();
        const char *name = m->toPrettyChars();
        m->namelen = strlen(name);
        dtb.nbytes(m->namelen + 1, name);
        //printf("nameoffset = x%x\n", nameoffset);
    }

    objc_Module_genmoduleinfo_classes();
    m->csym->Sdt = dtb.finish();
    out_readonly(m->csym);
    outdata(m->csym);

    //////////////////////////////////////////////

    objmod->moduleinfo(msym);
}

/* ================================================================== */

void toObjFile(Dsymbol *ds, bool multiobj)
{
    class ToObjFile : public Visitor
    {
    public:
        bool multiobj;

        ToObjFile(bool multiobj)
            : multiobj(multiobj)
        {
        }

        void visitNoMultiObj(Dsymbol *ds)
        {
            bool multiobjsave = multiobj;
            multiobj = false;
            ds->accept(this);
            multiobj = multiobjsave;
        }

        void visit(Dsymbol *ds)
        {
            //printf("Dsymbol::toObjFile('%s')\n", ds->toChars());
            // ignore
        }

        void visit(FuncDeclaration *fd)
        {
            // in glue.c
            FuncDeclaration_toObjFile(fd, multiobj);
        }

        void visit(ClassDeclaration *cd)
        {
            //printf("ClassDeclaration::toObjFile('%s')\n", cd->toChars());

            if (cd->type->ty == Terror)
            {
                cd->error("had semantic errors when compiling");
                return;
            }

            if (!cd->members)
                return;

            if (multiobj && !cd->hasStaticCtorOrDtor())
            {
                obj_append(cd);
                return;
            }

            if (global.params.symdebug)
                toDebug(cd);

            assert(!cd->_scope);     // semantic() should have been run to completion

            enum_SC scclass = SCcomdat;

            // Put out the members
            for (size_t i = 0; i < cd->members->dim; i++)
            {
                Dsymbol *member = (*cd->members)[i];
                /* There might be static ctors in the members, and they cannot
                 * be put in separate obj files.
                 */
                member->accept(this);
            }

            // Generate C symbols
            toSymbol(cd);
            toVtblSymbol(cd);
            Symbol *sinit = toInitializer(cd);

            //////////////////////////////////////////////

            // Generate static initializer
            {
                sinit->Sclass = scclass;
                sinit->Sfl = FLdata;
                DtBuilder dtb;
                ClassDeclaration_toDt(cd, dtb);
                sinit->Sdt = dtb.finish();
                out_readonly(sinit);
                outdata(sinit);
            }

            //////////////////////////////////////////////

            // Put out the TypeInfo
            genTypeInfo(cd->type, NULL);
            //toObjFile(cd->type->vtinfo, multiobj);

            //////////////////////////////////////////////

            // Put out the ClassInfo
            cd->csym->Sclass = scclass;
            cd->csym->Sfl = FLdata;

            /* The layout is:
               {
                    void **vptr;
                    monitor_t monitor;
                    byte[] m_init;              // static initialization data
                    string name;                // class name
                    void*[] vtbl;
                    Interface[] interfaces;
                    ClassInfo base;             // base class
                    void* destructor;
                    void function(Object) classInvariant;   // class invariant
                    ClassFlags m_flags;
                    void* deallocator;
                    OffsetTypeInfo[] offTi;
                    void function(Object) defaultConstructor;
                    //const(MemberInfo[]) function(string) xgetMembers;   // module getMembers() function
                    immutable(void)* m_RTInfo;
                    //TypeInfo typeinfo;
               }
             */
            unsigned offset = Target::classinfosize;    // must be ClassInfo.size
            if (Type::typeinfoclass)
            {
                if (Type::typeinfoclass->structsize != Target::classinfosize)
                {
        #ifdef DEBUG
                    printf("Target::classinfosize = x%x, Type::typeinfoclass->structsize = x%x\n", offset, Type::typeinfoclass->structsize);
        #endif
                    cd->error("mismatch between dmd and object.d or object.di found. Check installation and import paths with -v compiler switch.");
                    fatal();
                }
            }

            DtBuilder dtb;

            if (Type::typeinfoclass)            // vtbl for TypeInfo_Class : ClassInfo
                dtb.xoff(toVtblSymbol(Type::typeinfoclass), 0, TYnptr);
            else
                dtb.size(0);                    // BUG: should be an assert()
            dtb.size(0);                        // monitor

            // m_init[]
            assert(cd->structsize >= 8 || (cd->cpp && cd->structsize >= 4));
            dtb.size(cd->structsize);           // size
            dtb.xoff(sinit, 0, TYnptr);         // initializer

            // name[]
            const char *name = cd->ident->toChars();
            size_t namelen = strlen(name);
            if (!(namelen > 9 && memcmp(name, "TypeInfo_", 9) == 0))
            {
                name = cd->toPrettyChars();
                namelen = strlen(name);
            }
            dtb.size(namelen);
            dt_t *pdtname = dtb.xoffpatch(cd->csym, 0, TYnptr);

            // vtbl[]
            dtb.size(cd->vtbl.dim);
            if (cd->vtbl.dim)
                dtb.xoff(cd->vtblsym, 0, TYnptr);
            else
                dtb.size(0);

            // interfaces[]
            dtb.size(cd->vtblInterfaces->dim);
            if (cd->vtblInterfaces->dim)
                dtb.xoff(cd->csym, offset, TYnptr);      // (*)
            else
                dtb.size(0);

            // base
            if (cd->baseClass)
                dtb.xoff(toSymbol(cd->baseClass), 0, TYnptr);
            else
                dtb.size(0);

            // destructor
            if (cd->dtor)
                dtb.xoff(toSymbol(cd->dtor), 0, TYnptr);
            else
                dtb.size(0);

            // classInvariant
            if (cd->inv)
                dtb.xoff(toSymbol(cd->inv), 0, TYnptr);
            else
                dtb.size(0);

            // flags
            ClassFlags::Type flags = ClassFlags::hasOffTi;
            if (cd->isCOMclass()) flags |= ClassFlags::isCOMclass;
            if (cd->isCPPclass()) flags |= ClassFlags::isCPPclass;
            flags |= ClassFlags::hasGetMembers;
            flags |= ClassFlags::hasTypeInfo;
            if (cd->ctor)
                flags |= ClassFlags::hasCtor;
            for (ClassDeclaration *pc = cd; pc; pc = pc->baseClass)
            {
                if (pc->dtor)
                {
                    flags |= ClassFlags::hasDtor;
                    break;
                }
            }
            if (cd->isAbstract())
                flags |= ClassFlags::isAbstract;
            for (ClassDeclaration *pc = cd; pc; pc = pc->baseClass)
            {
                if (pc->members)
                {
                    for (size_t i = 0; i < pc->members->dim; i++)
                    {
                        Dsymbol *sm = (*pc->members)[i];
                        //printf("sm = %s %s\n", sm->kind(), sm->toChars());
                        if (sm->hasPointers())
                            goto L2;
                    }
                }
            }
            flags |= ClassFlags::noPointers;
          L2:
            dtb.size(flags);

            // deallocator
            if (cd->aggDelete)
                dtb.xoff(toSymbol(cd->aggDelete), 0, TYnptr);
            else
                dtb.size(0);

            // offTi[]
            dtb.size(0);
            dtb.size(0);            // null for now, fix later

            // defaultConstructor
            if (cd->defaultCtor && !(cd->defaultCtor->storage_class & STCdisable))
                dtb.xoff(toSymbol(cd->defaultCtor), 0, TYnptr);
            else
                dtb.size(0);

            // m_RTInfo
            if (cd->getRTInfo)
                Expression_toDt(cd->getRTInfo, dtb);
            else if (flags & ClassFlags::noPointers)
                dtb.size(0);
            else
                dtb.size(1);

            //dtb.xoff(toSymbol(cd->type->vtinfo), 0, TYnptr); // typeinfo

            //////////////////////////////////////////////

            // Put out (*vtblInterfaces)[]. Must immediately follow csym, because
            // of the fixup (*)

            offset += cd->vtblInterfaces->dim * (4 * Target::ptrsize);
            for (size_t i = 0; i < cd->vtblInterfaces->dim; i++)
            {
                BaseClass *b = (*cd->vtblInterfaces)[i];
                ClassDeclaration *id = b->sym;

                /* The layout is:
                 *  struct Interface
                 *  {
                 *      ClassInfo classinfo;
                 *      void*[] vtbl;
                 *      size_t offset;
                 *  }
                 */

                // Fill in vtbl[]
                b->fillVtbl(cd, &b->vtbl, 1);

                // classinfo
                dtb.xoff(toSymbol(id), 0, TYnptr);

                // vtbl[]
                dtb.size(id->vtbl.dim);
                dtb.xoff(cd->csym, offset, TYnptr);

                // offset
                dtb.size(b->offset);

                offset += id->vtbl.dim * Target::ptrsize;
            }

            // Put out the (*vtblInterfaces)[].vtbl[]
            // This must be mirrored with ClassDeclaration::baseVtblOffset()
            //printf("putting out %d interface vtbl[]s for '%s'\n", vtblInterfaces->dim, toChars());
            for (size_t i = 0; i < cd->vtblInterfaces->dim; i++)
            {
                BaseClass *b = (*cd->vtblInterfaces)[i];
                ClassDeclaration *id = b->sym;

                //printf("    interface[%d] is '%s'\n", i, id->toChars());
                size_t j = 0;
                if (id->vtblOffset())
                {
                    // First entry is ClassInfo reference
                    //dtb.xoff(toSymbol(id), 0, TYnptr);

                    // First entry is struct Interface reference
                    dtb.xoff(cd->csym, Target::classinfosize + i * (4 * Target::ptrsize), TYnptr);
                    j = 1;
                }
                assert(id->vtbl.dim == b->vtbl.dim);
                for (; j < id->vtbl.dim; j++)
                {
                    assert(j < b->vtbl.dim);
        #if 0
                    RootObject *o = b->vtbl[j];
                    if (o)
                    {
                        printf("o = %p\n", o);
                        assert(o->dyncast() == DYNCAST_DSYMBOL);
                        Dsymbol *s = (Dsymbol *)o;
                        printf("s->kind() = '%s'\n", s->kind());
                    }
        #endif
                    FuncDeclaration *fd = b->vtbl[j];
                    if (fd)
                    {
                        int offset = b->offset;
                        if (fd->interfaceVirtual)
                        {
                            offset -= fd->interfaceVirtual->offset;
                        }
                        dtb.xoff(toThunkSymbol(fd, offset), 0, TYnptr);
                    }
                    else
                        dtb.size(0);
                }
            }

            // Put out the overriding interface vtbl[]s.
            // This must be mirrored with ClassDeclaration::baseVtblOffset()
            //printf("putting out overriding interface vtbl[]s for '%s' at offset x%x\n", toChars(), offset);
            ClassDeclaration *pc;
            for (pc = cd->baseClass; pc; pc = pc->baseClass)
            {
                for (size_t k = 0; k < pc->vtblInterfaces->dim; k++)
                {
                    BaseClass *bs = (*pc->vtblInterfaces)[k];
                    FuncDeclarations bvtbl;
                    if (bs->fillVtbl(cd, &bvtbl, 0))
                    {
                        //printf("\toverriding vtbl[] for %s\n", bs->sym->toChars());
                        ClassDeclaration *id = bs->sym;

                        size_t j = 0;
                        if (id->vtblOffset())
                        {
                            // First entry is ClassInfo reference
                            //dtb.xoff(toSymbol(id), 0, TYnptr);

                            // First entry is struct Interface reference
                            dtb.xoff(toSymbol(pc), Target::classinfosize + k * (4 * Target::ptrsize), TYnptr);
                            offset += Target::ptrsize;
                            j = 1;
                        }

                        for (; j < id->vtbl.dim; j++)
                        {
                            assert(j < bvtbl.dim);
                            FuncDeclaration *fd = bvtbl[j];
                            if (fd)
                            {
                                int offset = bs->offset;
                                if (fd->interfaceVirtual)
                                {
                                    offset -= fd->interfaceVirtual->offset;
                                }
                                dtb.xoff(toThunkSymbol(fd, offset), 0, TYnptr);
                            }
                            else
                                dtb.size(0);
                            offset += Target::ptrsize;
                        }
                    }
                }
            }

            //////////////////////////////////////////////

            dtpatchoffset(pdtname, offset);

            dtb.nbytes(namelen + 1, name);
            const size_t namepad = -(namelen + 1) & (Target::ptrsize - 1); // align
            dtb.nzeros(namepad);

            cd->csym->Sdt = dtb.finish();
            // ClassInfo cannot be const data, because we use the monitor on it
            outdata(cd->csym);
            if (cd->isExport())
                objmod->export_symbol(cd->csym, 0);

            //////////////////////////////////////////////

            // Put out the vtbl[]
            //printf("putting out %s.vtbl[]\n", toChars());
            DtBuilder dtbv;
            if (cd->vtblOffset())
                dtbv.xoff(cd->csym, 0, TYnptr);           // first entry is ClassInfo reference
            for (size_t i = cd->vtblOffset(); i < cd->vtbl.dim; i++)
            {
                FuncDeclaration *fd = cd->vtbl[i]->isFuncDeclaration();

                //printf("\tvtbl[%d] = %p\n", i, fd);
                if (fd && (fd->fbody || !cd->isAbstract()))
                {
                    // Ensure function has a return value (Bugzilla 4869)
                    fd->functionSemantic();

                    Symbol *s = toSymbol(fd);

                    if (cd->isFuncHidden(fd))
                    {
                        /* fd is hidden from the view of this class.
                         * If fd overlaps with any function in the vtbl[], then
                         * issue 'hidden' error.
                         */
                        for (size_t j = 1; j < cd->vtbl.dim; j++)
                        {
                            if (j == i)
                                continue;
                            FuncDeclaration *fd2 = cd->vtbl[j]->isFuncDeclaration();
                            if (!fd2->ident->equals(fd->ident))
                                continue;
                            if (fd->leastAsSpecialized(fd2) || fd2->leastAsSpecialized(fd))
                            {
                                TypeFunction *tf = (TypeFunction *)fd->type;
                                if (tf->ty == Tfunction)
                                {
                                    cd->error("use of %s%s is hidden by %s; use 'alias %s = %s.%s;' to introduce base class overload set",
                                        fd->toPrettyChars(),
                                        parametersTypeToChars(tf->parameters, tf->varargs),
                                        cd->toChars(),

                                        fd->toChars(),
                                        fd->parent->toChars(),
                                        fd->toChars());
                                }
                                else
                                    cd->error("use of %s is hidden by %s", fd->toPrettyChars(), cd->toChars());
                                break;
                            }
                        }
                    }

                    dtbv.xoff(s, 0, TYnptr);
                }
                else
                    dtbv.size(0);
            }
            if (dtbv.isZeroLength())
            {
                /* Someone made an 'extern (C++) class C { }' with no virtual functions.
                 * But making an empty vtbl[] causes linking problems, so make a dummy
                 * entry.
                 */
                dtbv.size(0);
            }
            cd->vtblsym->Sdt = dtbv.finish();
            cd->vtblsym->Sclass = scclass;
            cd->vtblsym->Sfl = FLdata;
            out_readonly(cd->vtblsym);
            outdata(cd->vtblsym);
            if (cd->isExport())
                objmod->export_symbol(cd->vtblsym,0);
        }

        void visit(InterfaceDeclaration *id)
        {
            //printf("InterfaceDeclaration::toObjFile('%s')\n", id->toChars());

            if (id->type->ty == Terror)
            {
                id->error("had semantic errors when compiling");
                return;
            }

            if (!id->members)
                return;

            if (global.params.symdebug)
                toDebug(id);

            enum_SC scclass = SCcomdat;

            // Put out the members
            for (size_t i = 0; i < id->members->dim; i++)
            {
                Dsymbol *member = (*id->members)[i];
                visitNoMultiObj(member);
            }

            // Generate C symbols
            toSymbol(id);

            //////////////////////////////////////////////

            // Put out the TypeInfo
            genTypeInfo(id->type, NULL);
            id->type->vtinfo->accept(this);

            //////////////////////////////////////////////

            // Put out the ClassInfo
            id->csym->Sclass = scclass;
            id->csym->Sfl = FLdata;

            /* The layout is:
               {
                    void **vptr;
                    monitor_t monitor;
                    byte[] m_init;              // static initialization data
                    string name;                // class name
                    void*[] vtbl;
                    Interface[] interfaces;
                    ClassInfo base;             // base class
                    void* destructor;
                    void function(Object) classInvariant;   // class invariant
                    ClassFlags m_flags;
                    void* deallocator;
                    OffsetTypeInfo[] offTi;
                    void function(Object) defaultConstructor;
                    //const(MemberInfo[]) function(string) xgetMembers;   // module getMembers() function
                    immutable(void)* m_RTInfo;
                    //TypeInfo typeinfo;
               }
             */
            DtBuilder dtb;

            if (Type::typeinfoclass)
                dtb.xoff(toVtblSymbol(Type::typeinfoclass), 0, TYnptr); // vtbl for ClassInfo
            else
                dtb.size(0);                    // BUG: should be an assert()
            dtb.size(0);                        // monitor

            // m_init[]
            dtb.size(0);                        // size
            dtb.size(0);                        // initializer

            // name[]
            const char *name = id->toPrettyChars();
            size_t namelen = strlen(name);
            dtb.size(namelen);
            dt_t *pdtname = dtb.xoffpatch(id->csym, 0, TYnptr);

            // vtbl[]
            dtb.size(0);
            dtb.size(0);

            // interfaces[]
            unsigned offset = Target::classinfosize;
            dtb.size(id->vtblInterfaces->dim);
            if (id->vtblInterfaces->dim)
            {
                if (Type::typeinfoclass)
                {
                    if (Type::typeinfoclass->structsize != offset)
                    {
                        id->error("mismatch between dmd and object.d or object.di found. Check installation and import paths with -v compiler switch.");
                        fatal();
                    }
                }
                dtb.xoff(id->csym, offset, TYnptr);      // (*)
            }
            else
            {
                dtb.size(0);
            }

            // base
            assert(!id->baseClass);
            dtb.size(0);

            // destructor
            dtb.size(0);

            // classInvariant
            dtb.size(0);

            // flags
            ClassFlags::Type flags = ClassFlags::hasOffTi | ClassFlags::hasTypeInfo;
            if (id->isCOMinterface()) flags |= ClassFlags::isCOMclass;
            dtb.size(flags);

            // deallocator
            dtb.size(0);

            // offTi[]
            dtb.size(0);
            dtb.size(0);            // null for now, fix later

            // defaultConstructor
            dtb.size(0);

            // xgetMembers
            //dtb.size(0);

            // m_RTInfo
            if (id->getRTInfo)
                Expression_toDt(id->getRTInfo, dtb);
            else
                dtb.size(0);       // no pointers

            //dtb.xoff(toSymbol(id->type->vtinfo), 0, TYnptr); // typeinfo

            //////////////////////////////////////////////

            // Put out (*vtblInterfaces)[]. Must immediately follow csym, because
            // of the fixup (*)

            offset += id->vtblInterfaces->dim * (4 * Target::ptrsize);
            for (size_t i = 0; i < id->vtblInterfaces->dim; i++)
            {
                BaseClass *b = (*id->vtblInterfaces)[i];
                ClassDeclaration *base = b->sym;

                // classinfo
                dtb.xoff(toSymbol(base), 0, TYnptr);

                // vtbl[]
                dtb.size(0);
                dtb.size(0);

                // offset
                dtb.size(b->offset);
            }

            //////////////////////////////////////////////

            dtpatchoffset(pdtname, offset);

            dtb.nbytes(namelen + 1, name);
            const size_t namepad =  -(namelen + 1) & (Target::ptrsize - 1); // align
            dtb.nzeros(namepad);

            id->csym->Sdt = dtb.finish();
            out_readonly(id->csym);
            outdata(id->csym);
            if (id->isExport())
                objmod->export_symbol(id->csym, 0);
        }

        void visit(StructDeclaration *sd)
        {
            //printf("StructDeclaration::toObjFile('%s')\n", sd->toChars());

            if (sd->type->ty == Terror)
            {
                sd->error("had semantic errors when compiling");
                return;
            }

            if (multiobj && !sd->hasStaticCtorOrDtor())
            {
                obj_append(sd);
                return;
            }

            // Anonymous structs/unions only exist as part of others,
            // do not output forward referenced structs's
            if (!sd->isAnonymous() && sd->members)
            {
                if (global.params.symdebug)
                    toDebug(sd);

                genTypeInfo(sd->type, NULL);

                // Generate static initializer
                toInitializer(sd);
                if (sd->isInstantiated())
                {
                    sd->sinit->Sclass = SCcomdat;
                }
                else
                {
                    sd->sinit->Sclass = SCglobal;
                }

                sd->sinit->Sfl = FLdata;
                DtBuilder dtb;
                StructDeclaration_toDt(sd, dtb);
                sd->sinit->Sdt = dtb.finish();
                out_readonly(sd->sinit);    // put in read-only segment
                outdata(sd->sinit);

                // Put out the members
                for (size_t i = 0; i < sd->members->dim; i++)
                {
                    Dsymbol *member = (*sd->members)[i];
                    /* There might be static ctors in the members, and they cannot
                     * be put in separate obj files.
                     */
                    member->accept(this);
                }

                if (sd->xeq && sd->xeq != StructDeclaration::xerreq)
                    sd->xeq->accept(this);
                if (sd->xcmp && sd->xcmp != StructDeclaration::xerrcmp)
                    sd->xcmp->accept(this);
                if (sd->xhash)
                    sd->xhash->accept(this);
            }
        }

        void visit(VarDeclaration *vd)
        {

            //printf("VarDeclaration::toObjFile(%p '%s' type=%s) protection %d\n", vd, vd->toChars(), vd->type->toChars(), vd->protection);
            //printf("\talign = %d\n", vd->alignment);

            if (vd->type->ty == Terror)
            {
                vd->error("had semantic errors when compiling");
                return;
            }

            if (vd->aliassym)
            {
                visitNoMultiObj(vd->toAlias());
                return;
            }

            // Do not store variables we cannot take the address of
            if (!vd->canTakeAddressOf())
            {
                return;
            }

            if (!vd->isDataseg() || vd->storage_class & STCextern)
                return;

            Symbol *s = toSymbol(vd);
            unsigned sz = vd->type->size();

            Dsymbol *parent = vd->toParent();
            s->Sclass = SCglobal;

            do
            {
                /* Global template data members need to be in comdat's
                 * in case multiple .obj files instantiate the same
                 * template with the same types.
                 */
                if (parent->isTemplateInstance() && !parent->isTemplateMixin())
                {
                    s->Sclass = SCcomdat;
                    break;
                }
                parent = parent->parent;
            } while (parent);
            s->Sfl = FLdata;

            if (vd->_init)
            {
                DtBuilder dtb;
                Initializer_toDt(vd->_init, dtb);

                // Look for static array that is block initialized
                ExpInitializer *ie = vd->_init->isExpInitializer();

                Type *tb = vd->type->toBasetype();
                if (tb->ty == Tsarray && ie &&
                    !tb->nextOf()->equals(ie->exp->type->toBasetype()->nextOf()) &&
                    ie->exp->implicitConvTo(tb->nextOf())
                    )
                {
                    size_t dim = ((TypeSArray *)tb)->dim->toInteger();

                    // Duplicate Sdt 'dim-1' times, as we already have the first one
                    while (--dim > 0)
                    {
                        Expression_toDt(ie->exp, dtb);
                    }
                }
                s->Sdt = dtb.finish();
            }
            else
            {
                DtBuilder dtb;
                Type_toDt(vd->type, dtb);
                s->Sdt = dtb.finish();
            }

            // See if we can convert a comdat to a comdef,
            // which saves on exe file space.
            if (s->Sclass == SCcomdat &&
                s->Sdt &&
                dtallzeros(s->Sdt) &&
                !vd->isThreadlocal())
            {
                s->Sclass = SCglobal;
                dt2common(&s->Sdt);
            }

            if (!sz && vd->type->toBasetype()->ty != Tsarray)
                assert(0); // this shouldn't be possible

            if (sz || objmod->allowZeroSize())
            {
                outdata(s);
                if (vd->isExport())
                    objmod->export_symbol(s, 0);
            }
        }

        void visit(EnumDeclaration *ed)
        {
            if (ed->semanticRun >= PASSobj)  // already written
                return;
            //printf("EnumDeclaration::toObjFile('%s')\n", ed->toChars());

            if (ed->errors || ed->type->ty == Terror)
            {
                ed->error("had semantic errors when compiling");
                return;
            }

            if (ed->isAnonymous())
                return;

            if (global.params.symdebug)
                toDebug(ed);

            genTypeInfo(ed->type, NULL);

            TypeEnum *tc = (TypeEnum *)ed->type;
            if (!tc->sym->members || ed->type->isZeroInit())
                ;
            else
            {
                enum_SC scclass = SCglobal;
                if (ed->isInstantiated())
                    scclass = SCcomdat;

                // Generate static initializer
                toInitializer(ed);
                ed->sinit->Sclass = scclass;
                ed->sinit->Sfl = FLdata;
                DtBuilder dtb;
                Expression_toDt(tc->sym->defaultval, dtb);
                ed->sinit->Sdt = dtb.finish();
                outdata(ed->sinit);
            }
            ed->semanticRun = PASSobj;
        }

        void visit(TypeInfoDeclaration *tid)
        {
            if (isSpeculativeType(tid->tinfo))
            {
                //printf("-speculative '%s'\n", tid->toPrettyChars());
                return;
            }
            //printf("TypeInfoDeclaration::toObjFile(%p '%s') protection %d\n", tid, tid->toChars(), tid->protection);

            if (multiobj)
            {
                obj_append(tid);
                return;
            }

            Symbol *s = toSymbol(tid);
            s->Sclass = SCcomdat;
            s->Sfl = FLdata;

            DtBuilder dtb;
            TypeInfo_toDt(dtb, tid);
            s->Sdt = dtb.finish();

            // See if we can convert a comdat to a comdef,
            // which saves on exe file space.
            if (s->Sclass == SCcomdat &&
                dtallzeros(s->Sdt))
            {
                s->Sclass = SCglobal;
                dt2common(&s->Sdt);
            }

            outdata(s);
            if (tid->isExport())
                objmod->export_symbol(s, 0);
        }

        void visit(AttribDeclaration *ad)
        {
            Dsymbols *d = ad->include(NULL, NULL);

            if (d)
            {
                for (size_t i = 0; i < d->dim; i++)
                {
                    Dsymbol *s = (*d)[i];
                    s->accept(this);
                }
            }
        }

        void visit(PragmaDeclaration *pd)
        {
            if (pd->ident == Id::lib)
            {
                assert(pd->args && pd->args->dim == 1);

                Expression *e = (*pd->args)[0];

                assert(e->op == TOKstring);

                StringExp *se = (StringExp *)e;
                char *name = (char *)mem.xmalloc(se->numberOfCodeUnits() + 1);
                se->writeTo(name, true);

                /* Embed the library names into the object file.
                 * The linker will then automatically
                 * search that library, too.
                 */
                if (!obj_includelib(name))
                {
                    /* The format does not allow embedded library names,
                     * so instead append the library name to the list to be passed
                     * to the linker.
                     */
                    global.params.libfiles->push(name);
                }
            }
            else if (pd->ident == Id::startaddress)
            {
                assert(pd->args && pd->args->dim == 1);
                Expression *e = (*pd->args)[0];
                Dsymbol *sa = getDsymbol(e);
                FuncDeclaration *f = sa->isFuncDeclaration();
                assert(f);
                Symbol *s = toSymbol(f);
                obj_startaddress(s);
            }
            visit((AttribDeclaration *)pd);
        }

        void visit(TemplateInstance *ti)
        {
        #if LOG
            printf("TemplateInstance::toObjFile(%p, '%s')\n", ti, ti->toChars());
        #endif
            if (!isError(ti) && ti->members)
            {
                if (!ti->needsCodegen())
                {
                    //printf("-speculative (%p, %s)\n", ti, ti->toPrettyChars());
                    return;
                }
                //printf("TemplateInstance::toObjFile(%p, '%s')\n", ti, ti->toPrettyChars());

                if (multiobj)
                {
                    // Append to list of object files to be written later
                    obj_append(ti);
                }
                else
                {
                    for (size_t i = 0; i < ti->members->dim; i++)
                    {
                        Dsymbol *s = (*ti->members)[i];
                        s->accept(this);
                    }
                }
            }
        }

        void visit(TemplateMixin *tm)
        {
            //printf("TemplateMixin::toObjFile('%s')\n", tm->toChars());
            if (!isError(tm) && tm->members)
            {
                for (size_t i = 0; i < tm->members->dim; i++)
                {
                    Dsymbol *s = (*tm->members)[i];
                    s->accept(this);
                }
            }
        }

        void visit(StaticAssert *sa)
        {
        }

        void visit(Nspace *ns)
        {
        #if LOG
            printf("Nspace::toObjFile('%s', this = %p)\n", ns->toChars(), ns);
        #endif
            if (!isError(ns) && ns->members)
            {
                if (multiobj)
                {
                    // Append to list of object files to be written later
                    obj_append(ns);
                }
                else
                {
                    for (size_t i = 0; i < ns->members->dim; i++)
                    {
                        Dsymbol *s = (*ns->members)[i];
                        s->accept(this);
                    }
                }
            }
        }
    };

    ToObjFile v(multiobj);
    ds->accept(&v);
}

/******************************************
 * Get offset of base class's vtbl[] initializer from start of csym.
 * Returns ~0 if not this csym.
 */

unsigned baseVtblOffset(ClassDeclaration *cd, BaseClass *bc)
{
    //printf("ClassDeclaration::baseVtblOffset('%s', bc = %p)\n", cd->toChars(), bc);
    unsigned csymoffset = Target::classinfosize;    // must be ClassInfo.size
    csymoffset += cd->vtblInterfaces->dim * (4 * Target::ptrsize);

    for (size_t i = 0; i < cd->vtblInterfaces->dim; i++)
    {
        BaseClass *b = (*cd->vtblInterfaces)[i];

        if (b == bc)
            return csymoffset;
        csymoffset += b->sym->vtbl.dim * Target::ptrsize;
    }

    // Put out the overriding interface vtbl[]s.
    // This must be mirrored with ClassDeclaration::baseVtblOffset()
    //printf("putting out overriding interface vtbl[]s for '%s' at offset x%x\n", toChars(), offset);
    ClassDeclaration *cd2;

    for (cd2 = cd->baseClass; cd2; cd2 = cd2->baseClass)
    {
        for (size_t k = 0; k < cd2->vtblInterfaces->dim; k++)
        {
            BaseClass *bs = (*cd2->vtblInterfaces)[k];
            if (bs->fillVtbl(cd, NULL, 0))
            {
                if (bc == bs)
                {
                    //printf("\tcsymoffset = x%x\n", csymoffset);
                    return csymoffset;
                }
                csymoffset += bs->sym->vtbl.dim * Target::ptrsize;
            }
        }
    }

    return ~0;
}
