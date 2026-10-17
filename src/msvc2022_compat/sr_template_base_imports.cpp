/* msvc2022 port: local bodies for srClassSupport<> members that modern MSVC
   wants to import from SR.dll but retail SR.dll never exported.

   Why this exists
   ---------------
   Several SurRender classes are declared `class __declspec(dllimport) srCamera
   : public srClassSupport<srCamera, srNode, 0, 0x1400>`. Since VS 2005 (and in
   clang-cl), MSVC *propagates* dllimport from such a class to its base class
   template specialization. Every member of srClassSupport<srCamera, ...> that is
   not inlined is then called through __imp_<name>, i.e. expected as an SR.dll
   export. VC6 did not propagate: it instantiated those template members in the
   exe, and retail SR.dll indeed exports none of them (checked with dumpbin
   /exports on the retail DLL). Result: LNK2019 for sGetClassNode() of eight
   classes and for four virtuals of srClassSupport<srMaterial, ...> (emitted into
   the local srMaterial vtable by srMaterial's inline constructor).

   How it is fixed
   ---------------
   This file defines equivalent local bodies - line-for-line the
   srClassSupport<> template bodies from include/surrender/srTypeRegistry.h -
   and binds the missing names to them with the linker's /ALTERNATENAME:
     - for `__imp_X` (a pointer cell that `call [__imp_X]` reads) we provide a
       data cell holding the address of the local function;
     - for direct vtable references we alias the name to a local thiscall
       member function with the same signature.
   /ALTERNATENAME is only a fallback: it applies only if the name is still
   unresolved after every library was searched, so it never overrides a real
   import. No recovered source is changed. */

#include "surrender/srCamera.h"
#include "surrender/srClipPlane.h"
#include "surrender/srLight.h"
#include "surrender/srMaterial.h"
#include "surrender/srModel.h"
#include "surrender/srScene.h"
#include "surrender/srTexture.h"
#include "surrender/srTextureMap.h"

namespace {

typedef srRegistry::ClassNode* (*ParentNodeFn)();

/* srClassSupport<Derived, Base, RegisterInstances, ClassID>::sGetClassNode(),
   with Base::sGetClassNode passed explicitly so no call goes through an
   import that does not exist. */
template <class Derived, bool RegisterInstances, w8_ulong ClassID, ParentNodeFn Parent>
srRegistry::ClassNode* __cdecl LocalClassNode()
{
    srRegistry* registry = srCore.getRegistry();
    srRegistry::ClassNode* node = registry->getClassNode(ClassID);

    if (node == 0) {
        node = registry->registerClass(Derived::sGetClassName(), Parent(), ClassID,
                                       RegisterInstances);
    }
    return node;
}

srRegistry::ClassNode* __cdecl ClassNodeOfSrClass()
{
    return srClass::sGetClassNode(); /* real SR.dll export */
}

srRegistry::ClassNode* __cdecl ClassNodeOfSrNode()
{
    return srNode::sGetClassNode(); /* srNode is not dllimport: local instance */
}

srRegistry::ClassNode* __cdecl ClassNodeOfSrIlluminator()
{
    return srIlluminator::sGetClassNode(); /* not dllimport: local instance */
}

} // namespace

/* Ordering mirrors the class IDs in the template arguments of each header. */
#define WIZ8_LOCAL_NODE(Derived, Reg, Id, Parent) LocalClassNode<Derived, Reg, Id, Parent>

/* Non-const on purpose: a namespace-scope const would get internal linkage
   and the linker could not see the cell (it plays the role of an IAT slot). */
extern "C" {
/* srMaterialIFace / srTextureIFace are dllimport too; their nodes are parents
   of srMaterial / srTexture, so keep a local body for them as well. */
void* wiz8_imp_srMaterialIFace_sGetClassNode =
    reinterpret_cast<void*>(&WIZ8_LOCAL_NODE(srMaterialIFace, true, 0x2200, ClassNodeOfSrClass));
void* wiz8_imp_srTextureIFace_sGetClassNode =
    reinterpret_cast<void*>(&WIZ8_LOCAL_NODE(srTextureIFace, true, 0x2100, ClassNodeOfSrClass));
}

namespace {
srRegistry::ClassNode* __cdecl ClassNodeOfSrMaterialIFace()
{
    return LocalClassNode<srMaterialIFace, true, 0x2200, ClassNodeOfSrClass>();
}
srRegistry::ClassNode* __cdecl ClassNodeOfSrTextureIFace()
{
    return LocalClassNode<srTextureIFace, true, 0x2100, ClassNodeOfSrClass>();
}
srRegistry::ClassNode* __cdecl ClassNodeOfSrTexture()
{
    return LocalClassNode<srTexture, false, 0x2110, ClassNodeOfSrTextureIFace>();
}
srRegistry::ClassNode* __cdecl ClassNodeOfSrMaterial()
{
    return LocalClassNode<srMaterial, false, 0x2210, ClassNodeOfSrMaterialIFace>();
}
} // namespace

extern "C" {
void* wiz8_imp_srCamera_sGetClassNode =
    reinterpret_cast<void*>(&WIZ8_LOCAL_NODE(srCamera, false, 0x1400, ClassNodeOfSrNode));
void* wiz8_imp_srClipPlane_sGetClassNode =
    reinterpret_cast<void*>(&WIZ8_LOCAL_NODE(srClipPlane, false, 0x1500, ClassNodeOfSrNode));
void* wiz8_imp_srScene_sGetClassNode =
    reinterpret_cast<void*>(&WIZ8_LOCAL_NODE(srScene, false, 0x1010, ClassNodeOfSrNode));
void* wiz8_imp_srLight_sGetClassNode =
    reinterpret_cast<void*>(&WIZ8_LOCAL_NODE(srLight, false, 0x1220, ClassNodeOfSrIlluminator));
void* wiz8_imp_srModel_sGetClassNode =
    reinterpret_cast<void*>(&WIZ8_LOCAL_NODE(srModel, true, 0x2000, ClassNodeOfSrClass));
void* wiz8_imp_srTexture_sGetClassNode = reinterpret_cast<void*>(&ClassNodeOfSrTexture);
void* wiz8_imp_srTextureMap_sGetClassNode =
    reinterpret_cast<void*>(&WIZ8_LOCAL_NODE(srTextureMap, false, 0x2111, ClassNodeOfSrTexture));
void* wiz8_imp_srMaterial_sGetClassNode = reinterpret_cast<void*>(&ClassNodeOfSrMaterial);
}

#define WIZ8_ALIAS(from, to) __pragma(comment(linker, "/alternatename:" from "=" to))

// clang-format off
WIZ8_ALIAS("__imp_?sGetClassNode@?$srClassSupport@VsrMaterialIFace@@VsrClass@@$00$0CCAA@@@SAPAVClassNode@srRegistry@@XZ", "_wiz8_imp_srMaterialIFace_sGetClassNode")
WIZ8_ALIAS("__imp_?sGetClassNode@?$srClassSupport@VsrTextureIFace@@VsrClass@@$00$0CBAA@@@SAPAVClassNode@srRegistry@@XZ", "_wiz8_imp_srTextureIFace_sGetClassNode")
WIZ8_ALIAS("__imp_?sGetClassNode@?$srClassSupport@VsrCamera@@VsrNode@@$0A@$0BEAA@@@SAPAVClassNode@srRegistry@@XZ", "_wiz8_imp_srCamera_sGetClassNode")
WIZ8_ALIAS("__imp_?sGetClassNode@?$srClassSupport@VsrClipPlane@@VsrNode@@$0A@$0BFAA@@@SAPAVClassNode@srRegistry@@XZ", "_wiz8_imp_srClipPlane_sGetClassNode")
WIZ8_ALIAS("__imp_?sGetClassNode@?$srClassSupport@VsrScene@@VsrNode@@$0A@$0BABA@@@SAPAVClassNode@srRegistry@@XZ", "_wiz8_imp_srScene_sGetClassNode")
WIZ8_ALIAS("__imp_?sGetClassNode@?$srClassSupport@VsrLight@@VsrIlluminator@@$0A@$0BCCA@@@SAPAVClassNode@srRegistry@@XZ", "_wiz8_imp_srLight_sGetClassNode")
WIZ8_ALIAS("__imp_?sGetClassNode@?$srClassSupport@VsrModel@@VsrClass@@$00$0CAAA@@@SAPAVClassNode@srRegistry@@XZ", "_wiz8_imp_srModel_sGetClassNode")
WIZ8_ALIAS("__imp_?sGetClassNode@?$srClassSupport@VsrTexture@@VsrTextureIFace@@$0A@$0CBBA@@@SAPAVClassNode@srRegistry@@XZ", "_wiz8_imp_srTexture_sGetClassNode")
WIZ8_ALIAS("__imp_?sGetClassNode@?$srClassSupport@VsrTextureMap@@VsrTexture@@$0A@$0CBBB@@@SAPAVClassNode@srRegistry@@XZ", "_wiz8_imp_srTextureMap_sGetClassNode")
WIZ8_ALIAS("__imp_?sGetClassNode@?$srClassSupport@VsrMaterial@@VsrMaterialIFace@@$0A@$0CCBA@@@SAPAVClassNode@srRegistry@@XZ", "_wiz8_imp_srMaterial_sGetClassNode")
// clang-format on

/* The non-overridden srClassSupport<srMaterial, srMaterialIFace, 0, 0x2210>
   virtuals in the exe-local srMaterial vtable. `this` is the srMaterial object:
   srClassSupport<srMaterial, ...> is its only base, at offset 0. Each body is
   the template's; the alias pragma sits inside the body so __FUNCDNAME__ (the
   decorated name of this very member) can be used as the alias target. */
class Wiz8SrMaterialSupportThunks {
public:
    const char* MaterialClassName() const;
    w8_ulong MaterialClassID() const;
    srRegistry::ClassNode* MaterialClassNode() const;
    srClass* MaterialClone();

private:
    const srMaterial* Self() const
    {
        return reinterpret_cast<const srMaterial*>(this);
    }
    srMaterial* Self()
    {
        return reinterpret_cast<srMaterial*>(this);
    }
};

const char* Wiz8SrMaterialSupportThunks::MaterialClassName() const
{
    WIZ8_ALIAS("?getClassName@?$srClassSupport@VsrMaterial@@VsrMaterialIFace@@$0A@$0CCBA@@@UBEPBDXZ", __FUNCDNAME__)
    return srMaterial::sGetClassName();
}

w8_ulong Wiz8SrMaterialSupportThunks::MaterialClassID() const
{
    WIZ8_ALIAS("?getClassID@?$srClassSupport@VsrMaterial@@VsrMaterialIFace@@$0A@$0CCBA@@@UBEKXZ", __FUNCDNAME__)
    return 0x2210;
}

srRegistry::ClassNode* Wiz8SrMaterialSupportThunks::MaterialClassNode() const
{
    WIZ8_ALIAS("?getClassNode@?$srClassSupport@VsrMaterial@@VsrMaterialIFace@@$0A@$0CCBA@@@UBEPAVClassNode@srRegistry@@XZ", __FUNCDNAME__)
    return ClassNodeOfSrMaterial();
}

srClass* Wiz8SrMaterialSupportThunks::MaterialClone()
{
    WIZ8_ALIAS("?vClone@?$srClassSupport@VsrMaterial@@VsrMaterialIFace@@$0A@$0CCBA@@@UAEPAVsrClass@@XZ", __FUNCDNAME__)
    srMaterial* copy = static_cast<srMaterial*>(Self()->vInstance());
    *copy = *Self();
    return copy;
}
