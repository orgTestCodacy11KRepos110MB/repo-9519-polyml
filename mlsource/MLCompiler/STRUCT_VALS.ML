(*
    Copyright (c) 2000
        Cambridge University Technical Services Limited

    Modified David C. J. Matthews 2009, 2012, 2015.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License version 2.1 as published by the Free Software Foundation.
    
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*)

(*
    Title:      Global and Local values.
    Author:     Dave Matthews,Cambridge University Computer Laboratory
    Copyright   Cambridge University 1985
*)

(* This type contains the basic structures of global and local declarations.
   Putting the global declarations in a separate type allows us to install a new
   compiler (particularly to fix bugs) and still be compatible with declarations
   made with the old compiler. It is also convenient to put local values in
   here as well.  *) 
  
functor STRUCT_VALS (

structure CODETREE : CODETREE where type machineWord = Address.machineWord

(*****************************************************************************)
(*                  UNIVERSALTABLE                                           *)
(*****************************************************************************)
structure UNIVERSALTABLE :
sig
  type 'a tag = 'a Universal.tag;
  type univTable
  type universal = Universal.universal
  
  val makeUnivTable: unit -> univTable;
  val univEnter:     univTable * 'a tag * string * 'a -> unit;
  val univLookup:    univTable * 'a tag * string -> 'a option;
  
  (* Freeze a mutable table so it is immutable. *)
  val univFreeze:       univTable -> univTable

    val fold: (string * universal * 'a -> 'a) -> 'a -> univTable -> 'a
end

) :> STRUCTVALSIG where type codetree = CODETREE.codetree and type univTable = UNIVERSALTABLE.univTable and type level = CODETREE.level
=  

(*****************************************************************************)
(*                  STRUCTVALS functor body                                  *)
(*****************************************************************************)
struct
    open CODETREE;
  
    open Misc;
    open Universal;
    open UNIVERSALTABLE;
  
    (* Location for declarations. *)
    type location =
        { file: string, startLine: FixedInt.int, startPosition: FixedInt.int,
          endLine: FixedInt.int, endPosition: FixedInt.int }

    (* The idea of this is reduce the number of mutable objects. *)
    datatype 'a possRef = FrozenRef of 'a | VariableRef of 'a ref
    fun pling(FrozenRef x) = x | pling(VariableRef(ref x)) = x
    fun updatePR(VariableRef r, x) = r := x | updatePR(FrozenRef _, _) = raise Fail "Assignment to frozen ref"

    (* References to identifiers.  exportedRef is set to true if the identifier is exported to
       the global environment or to a structure.  localRef contains the list of local uses.
       recursiveRef contains a list of (mutually) recursive references together with the name
       of the function that refers to this.  It is used when computing whether a recursive
       function is actually refered to from elsewhere. 
       This is an option type because this is used only within local identifiers. *)
    type references =
    {
        exportedRef: bool ref,
        localRef: location list ref,
        recursiveRef: (location * string) list ref
    } option
    
    fun makeRef(): references =
        SOME { exportedRef = ref false, localRef = ref nil, recursiveRef=ref nil }
   
  (* typeIds are used to distinguish between concrete types.  Two
     types that share will have the same identifier.  If the identifiers are
     different they are different types.
     There are three classes of type identifier.  Free identifiers are used for
     types in the global environment. Bound identifiers occur in signatures,
     functors and while compiling structures.  Type functions arise from
     type bindings (type abbreviations) or from "where type" definitions
     in signatures.
     The type identifier also contains the equality attribute.
     In ML97 only types have these identifiers.  In ML90 these were also
     needed for structures.
     Free and Bound IDs contain information to find the equality and
     value-printing functions. *)
    type typeIdDescription = { location: location, name: string, description: string }

    datatype typeId =
        TypeId of { access: valAccess, description: typeIdDescription, idKind: typeIdKind }

    and typeIdKind =
        Free of { uid: uniqueId, allowUpdate: bool, arity: int  }
    |   Bound of { offset: int, eqType: bool possRef, isDatatype: bool, arity: int }
    |   TypeFn of typeVarForm list * types

    (* A type is the union of these different cases. *)
    and types = 
        TypeVar          of typeVarForm

    |   TypeConstruction of
        {
            name:  string,
            constr: typeConstrs,
            args:  types list,
            locations: locationProp list
        }

    |   FunctionType of
        { 
            arg:    types,
            result: types
        }

    |   LabelledType  of labelledRec

    |   OverloadSet   of
        {
            typeset: typeConstrs list
        }

    |   BadType

    |   EmptyType

    and typeConstrs = 
        TypeConstrs of
        {
            name:       string,
            typeVars:   typeVarForm list,
            identifier: typeId,
            locations:  locationProp list (* Location of declaration *)
        }

    and typeConstrSet = (* A type constructor with its, possible, value constructors. *)
        TypeConstrSet of typeConstrs * values list

    and labelFieldList =
        FieldList of string list * bool (* True if this is frozen *)
    |   FlexibleList of labelFieldList ref

    (* Access to a value, structure or functor. *)
    and valAccess =
        Global   of codetree
    |   Local    of { addr: int ref, level: level ref }
    |   Selected of { addr: int,     base:  valAccess }
    |   Formal   of int
    |   Overloaded of typeDependent (* Values only. *)

    (* Structures. *)
    and structVals = 
       Struct of
        {
            name:   string,
            signat: signatures,
            access: valAccess,
            locations: locationProp list
        }

    (* Signatures.  The representation of a signature deserves a bit of explanation.
       A signature is an environment: a set of values, type constructors and sub-structures.
       Behind the types associated with the type constructors and values
       are what the semantics calls "type names" but are referred to in the Poly/ML
       code as type-ids to avoid confusion with the text name of a type constructor.
       The same signature may be bound to a signature identifier, used as an argument
       to a functor, the result of a functor or the signature of a structure.  The
       environment is the same in each case; what is different is the type names.
       To avoid rebuilding the environment for each case the same environment is
       used but with different sets of typeIdMap, firstBoundIndex and boundIds.
       typeIdMap maps the "offset" of any bound id found in the environment to a
       type-id.  For structures the result will always be a free id and boundIds
       will be empty.  In other cases it may map to either a free id, perhaps as
       the result of "where type" constraint, or to a bound id.  Sharing constraints
       will cause different bound ids in the environment to map to the same resulting
       bound id.  Normally, firstBoundIndex will be zero and boundIds will be the
       set of bound ids that can be produced by typeIdMap.  The exception is the
       result signature of a functor.  In that case typeIdMap may return bound Ids
       in the set of the boundIds for the argument to the functor, which will have
       offsets >= 0 and < firstBoundIndex or in the set for the result of the functor
       with offsets >= firstBoundIndex and < firstBoundIndex+length boundIds.  When
       the functor is applied the typeIdMap for the structure that is produced
       maps the first set to the free ids of the actual argument and the maps the
       second set to new, unique free ids. *)
    and signatures =
        Signatures of
        { 
            name:               string,
            tab:                univTable,
            typeIdMap:          int -> typeId,
            firstBoundIndex:    int,
            boundIds:           typeId list,
            locations:          locationProp list
        }

    and functors =
        Functor of
        {
            name:       string,
            arg:        structVals,
            result:     signatures,
            access:     valAccess,
            locations:  locationProp list
        }

    (* Values. *)
    (* The overloaded functions divide up into basically two groups: Those =, 
       <>, print and makestring  which are infinitely overloaded and those 
       *, + etc  which are overloaded on a limited range of types. *)  
    and typeDependent =
        Print
    |   GetPretty
    |   MakeString
    |   AddPretty
    |   Equal
    |   NotEqual
    |   AddOverload
    |   TypeDep
    |   GetLocation

    and values =
        Value of
        {
            name: string,
            typeOf: types,
            access: valAccess,
            class: valueClass,
            locations: locationProp list, (* Location of declaration *)
            references: references,
            instanceTypes: types list ref option (* Instance types for local variables. *)
        }

    (* Classes of values. *)
    and valueClass =
        ValBound
    |   PattBound
    |   Exception
    |   Constructor of
        {
            nullary: bool, (* True if this is a single value (e.g. "nil") rather than a function. *)
            ofConstrs: int (* Total number of constructors in the datatype. *)
        }

    and locationProp =
        DeclaredAt of location
    |   OpenedAt of location
    |   StructureAt of location
    |   SequenceNo of FixedInt.int
  
    withtype uniqueId = bool ref
        (* We use a ref here both because we can then set equality if we
           need but also because it allows us to create a unique Id. *)
      
    and typeVarForm = 
    {
        value:    types ref,
        encoding: Word.word
    }

    and labelledRec =
    {
        (* Fields actually present in this record.  If this was flexible at some
           stage there may be extra fields listed in the full field list. *)
        recList: { name: string, typeof: types } list,
        (* The names of all the fields including extra fields. *)
        fullList: labelFieldList
    }

    
  (* A set of type contructors.  This is used only during the
     compilation process and represents the set of possible types
     which may occur. It functions in much the same way as a type
     variable.  Because we only allow overloading on monomorphic
     type constructors such as "int" and "word" we can restrict the
     set to containing only type constructors rather than general types.
     This overload set was added for ML 97 because ML 97, unlike ML 90,
     defaults overloaded operators and constants if unification does
     not result in a single type being found.  
     The overload set is used in a similar way to a flexible record
     and will always be pointed at by a type variable so that the
     set can be replaced by a single type construction if the unification
     reduces to a single type. *)
    and overloadSetForm =
    {
        typeset: typeConstrs list
    }

    (* Identifiers *)
    fun makeFreeId(arity, access, eq, desc) =
        TypeId { access=access, description = desc,
            idKind = Free {uid = ref eq, allowUpdate=false, arity=arity}}
    (* At the moment the only reason for distinguishing makeFreeId and makeFreeIdEqUpdate
       is that it allows us to check that we're actually permitting update when needed. *)
    fun makeFreeIdEqUpdate(arity, access, eq, desc) =
        TypeId { access=access, description = desc,
            idKind = Free {uid = ref eq, allowUpdate=true, arity=arity}}

    fun makeBoundId (arity, access, n, eq, isdt, desc) =
        TypeId { access=access, description = desc,
            idKind = Bound{offset=n, eqType=FrozenRef eq, isDatatype = isdt, arity=arity}}

    (* Within the body of a functor we make bound stamps but may need to
       set the equality attribute. *)
    fun makeBoundIdWithEqUpdate (arity, access, n, eq, isdt, desc) =
        TypeId { access=access, description = desc,
            idKind = Bound{offset=n, eqType=VariableRef(ref eq), isDatatype = isdt, arity=arity}}

    (* Type functions currently always have Free ids. *)
    fun makeTypeFunction(desc, typeFn) =
        TypeId { access=Global CodeZero, description = desc, idKind = TypeFn typeFn};
            
    (* Find the number - assuming it is bound. *)
    fun offsetId (TypeId{idKind=Bound {offset, ...}, ...}) = offset
    |   offsetId _       = raise InternalError "offsetId: not a Bound";

    (* Are two type constructors the same? *)
    fun sameTypeId (TypeId{idKind=Free{uid = a, ...}, ...}, TypeId{idKind=Free {uid = b, ...}, ...}) = a = b
    |   sameTypeId (TypeId{idKind=Bound{offset=a, ...}, ...}, TypeId{idKind=Bound{offset=b, ...}, ...}) = a = b
    |   sameTypeId _ = false (* Includes type functions. *)

    fun idAccess (TypeId { access, ...}) = access

    fun isEquality (TypeId { idKind = Free{uid = ref eq, ...}, ...}) = eq
    |   isEquality (TypeId { idKind = Bound{eqType, ...}, ...}) = pling eqType
    |   isEquality (TypeId { idKind = TypeFn _, ...}) = raise InternalError "isEquality: TypeFn"

    (* Set the equality property.   Currently, free IDs are used for abstypes and
       datatypes that are local to a function as well as the usual case of using them
       for top-level types. *)
    fun setEquality(TypeId{idKind = Free{uid, allowUpdate=true, ...}, ...}, eq) = uid := eq
    |   setEquality(TypeId{idKind = Bound{eqType=VariableRef id, ...}, ...}, eq) = id := eq
    |   setEquality _ = raise InternalError "setEquality: can't set equality attribute"

    (* Signatures: Used for both signatures of local structures and for global structures 
       (name spaces). Strictly signatures do not contain fix-status functors
       or signatures but as we use these structures for top-level name-spaces
       we have to have tables for these. *)
    val makeSignatureTable = makeUnivTable

    (* Make a signature, freezing the table. *)
    fun makeSignature (name, table, fbi, locations, typeIdMap, boundIds) =
        Signatures { name = name,
               tab        = univFreeze table,
               typeIdMap  = typeIdMap,
               firstBoundIndex   = fbi,
               boundIds   = boundIds,
               locations = locations  }
    
  (* Types. *)

  (* Level at which type is generalisable. *)

  val generalisable = 9999; 
    
    
  (* Destructors, constructors and predicates for types *)
  val emptyType            = EmptyType;
  val badType              = BadType;

  fun isEmpty             EmptyType           = true | isEmpty            _ = false;
  fun isBad               BadType             = true | isBad              _ = false;
  
  fun makeValueConstr (name, typeOf, nullary, constrs, access, locations) : values =
    Value
    { 
      name    = name,
      typeOf  = typeOf,
      access  = access,
      class   = Constructor { nullary = nullary, ofConstrs = constrs },
      locations = locations,
      references = NONE,
      instanceTypes = NONE
    };

  
  (* A type variable is implemented as a true variable i.e. it can
     be assigned a particular type when it is unified. Initially it is
     set to EmptyType which represents an unset type variable.
     When it is unified with a type it is set to point to the type it
     has been unified with.  Type variables associated with variables
     have level set to the nesting level, others have level set to
     "generalisable". If two type variables are united their levels are 
     set to the lower of the two. If level is not "generalisable" the type
     variable is not generalisable. This is needed to deal with cases like
       fn a => let val x = a in x end      and
       fn a => let val x = hd a in x end
     The level is set to "generalisable" at the end of the block with that
     level. Actually ``level'' is not actually changed - instead the type
     variable is assigned to a new variable with the correct level, since
     only the last variable in a sequence is looked at.
     ``equality'' is true if this is an equality variable e.g. ''a.
     ``nonunifiable'' is true for type variables introduced explicitly
     or type variables in signatures. Such type variables can have their
     level changed but cannot be unified with other types, with other
     nonunifiable type variables or with equality variables (unless it
     is already an equality variable). 
     ``weak'' is true if this is an imperative type variable e.g. '_a *)

    fun sameTv (a : typeVarForm, b : typeVarForm) : bool = 
        #value a = #value b; (* If the same ref it must be the same *)
        
    local
        open Word
        infix 8 >> <<
        infix 7 andb
        infix 6 orb
    in
        fun makeTv {value : types, level, equality, nonunifiable, printable} : typeVarForm =
            { value    = ref value, (* REF HOTSPOT - 400 *)
              encoding = (fromInt level << 0w3)
                           orb (if equality     then 0w4 else 0w0)
                           orb (if nonunifiable then 0w2 else 0w0)
                           orb (if printable    then 0w1 else 0w0)}
        
        fun tvSetValue ({ value, ...} : typeVarForm, t : types) = value := t
        fun tvValue ({value = ref v, ...} : typeVarForm) : types = v
        fun tvLevel ({encoding, ...} : typeVarForm) : int  = Word.toInt(encoding >> 0w3)
        fun tvEquality ({encoding, ...} : typeVarForm)     = encoding andb 0w4 <> 0w0
        fun tvNonUnifiable ({encoding, ...} : typeVarForm) = encoding andb 0w2 <> 0w0
        fun tvPrintity ({encoding, ...} : typeVarForm)     = encoding andb 0w1 <> 0w0
    end

    local
        fun follow (FlexibleList(ref r)) = follow r
        |   follow (FieldList c) = c
    in
        fun recordIsFrozen { fullList, ...} =
            #2 (follow fullList)
        and recordFields {fullList, ...} =
            #1 (follow fullList)
    end

  (* Type constructors are identifiers which take zero or more types and yield a
     type as result. Their main property is that two type constructors can be 
     unified iff they are the same constructor. Another use for
     constructors is for aliasing types. In this case "typeVars" points to a list 
     of type variables which are used in the "equivalent" type. ``equality'' is a 
     flag indicating if the values can be tested for equality. *)
      
    fun tcName       (TypeConstrs {name,...})       = name
    fun tcTypeVars   (TypeConstrs {typeVars,...})   = typeVars
    fun tcIdentifier (TypeConstrs {identifier,...}) = identifier
    fun tcLocations  (TypeConstrs {locations, ...}) = locations

    (* Is this a type function?  N.B. It is possible, though unlikely, that it
       is a datatype as well i.e. has value constructors. *)
    fun tcIsAbbreviation (TypeConstrs {identifier = TypeId{idKind = TypeFn _, ...},...}) = true
    |   tcIsAbbreviation _ = false

    fun tcArity(TypeConstrs {identifier=TypeId{idKind=TypeFn(args, _),...}, ...}) = length args
    |   tcArity(TypeConstrs {identifier=TypeId{idKind=Bound{arity, ...},...}, ...}) = arity
    |   tcArity(TypeConstrs {identifier=TypeId{idKind=Free{arity, ...},...}, ...}) = arity

    (* Equality and "equivalence" are now properties of the type id.  Retain these functions for the moment. *)

    val tcEquality = isEquality o tcIdentifier;
    fun tcSetEquality(tc, eq) = setEquality(tcIdentifier tc, eq)

    (* Construct a type constructor.  We need typeVars here purely to be
       able to print datatypes with equality type variables e.g. datatype ''a t. *)
    fun makeTypeConstructor (name, typeVars, uid, locations) =
        TypeConstrs
        {
            name       = name,
            typeVars   = typeVars,
            identifier = uid,
            locations = locations
        }

    fun tsConstr(TypeConstrSet(ts, _)) = ts
    and tsConstructors(TypeConstrSet(_, tvs)) = tvs

    val inBasis =
        { file = "Standard Basis", startLine = 0, startPosition = 0, endLine = 0, endPosition = 0}
    fun basisDescription name = { location = inBasis, description = "In Basis", name = name }


    (* Infix status. *)
    datatype infixity = 
        Infix of int
    |   InfixR of int
    |   Nonfix

    datatype fixStatus = FixStatus of string * infixity

    fun vaGlobal   (Global   x) = x | vaGlobal   _ = raise Match
    fun vaLocal    (Local    x) = x | vaLocal    _ = raise Match

    val makeGlobal = Global;
    val makeFormal = Formal;
  
    fun makeLocal () = Local { addr = ref ~1 (* Invalid addr - catch errors *), level = ref baseLevel }
       
    fun makeSelected (addr, Struct{access, ...}) = Selected { addr = addr, base = access }

    fun makeStruct (name, signat, access, locations) = 
        Struct { name = name, signat = signat, access = access, locations = locations }
    
    (* Global structure *)
    fun makeGlobalStruct (name, signat, code, locations) =
        makeStruct (name, signat, makeGlobal code, locations)

    (* These are used in INITIALISE so must be mutable. *)
    fun makeEmptyGlobal name =
        makeStruct (name,
            Signatures { name = "",
                   tab        = makeUnivTable(),
                   typeIdMap  = fn _ => raise Subscript,
                   firstBoundIndex   = 0, 
                   boundIds   = [],
                   locations =  [DeclaredAt inBasis] },
            makeGlobal CodeZero, [DeclaredAt inBasis])
     
    (* Local structure. *)
    fun makeLocalStruct (name, signat, location) = 
        makeStruct (name, signat, makeLocal (), location);
     
    (* Structure in a local structure or a functor argument. *)
    fun makeSelectedStruct (selected as Struct{access, name, signat, locations, ...}, base, openLocs) =
    case access of 
        Formal sel =>
           makeStruct(name, signat, makeSelected (sel, base), openLocs @ locations)
      | Global code => (* Need to add the locations. *)
           makeStruct(name, signat, Global code, openLocs @ locations)
      | _          => selected
  
    fun makeFormalStruct (name, signat, addr, location) =
      makeStruct (name, signat, makeFormal addr, location);
     
    (* Values. *)  
    fun makeOverloaded (name, typeOf, operation) : values =
    Value{ name = name, typeOf = typeOf, access = Overloaded operation, class = ValBound,
           locations = [DeclaredAt inBasis], references = NONE, instanceTypes = NONE};

    val undefinedValue    =
    Value{ name = "<undefined>", typeOf = BadType, access = Global CodeZero, class = ValBound,
           locations = [DeclaredAt inBasis], references = NONE, instanceTypes = NONE };

    fun isUndefinedValue(Value{name = "<undefined>", ...}) = true | isUndefinedValue _ = false

    fun valName (Value{name, ...}) = name
    fun valTypeOf (Value{typeOf, ...}) = typeOf

    fun isConstructor (Value{class=Constructor _, ...}) = true
    | isConstructor (Value{class=Exception, ...})     = true
    | isConstructor _                                  = false;

    fun isValueConstructor (Value{class=Constructor _, ...}) = true
    | isValueConstructor _                                 = false;


    (* Functor value. *)
    fun makeFunctor (name, arg, result, access, locations) = 
        Functor 
        {
            name = name,
            arg = arg,
            result = result,
            access = access,
            locations = locations
        }

    val valueVar:      values      tag = tag();
    val typeConstrVar: typeConstrSet tag = tag();
    val fixVar:        fixStatus   tag = tag();
    val structVar:     structVals  tag = tag();
    val signatureVar:  signatures  tag = tag();
    val functorVar:    functors    tag = tag();

    fun makeLook (t:'a tag) table n = univLookup (table, t, n)
    and makeEnter (t:'a tag) table (n, v) = univEnter (table, t, n, v)
    and makeAllNames (t:'a tag) table () =
        UNIVERSALTABLE.fold (fn (s, u, l) => if Universal.tagIs t u then s :: l else l) [] table

    datatype env = 
    Env of 
    { 
        lookupVal:    string -> values option,
        lookupType:   string -> typeConstrSet option,
        lookupFix:    string -> fixStatus option,
        lookupStruct: string -> structVals option,
        lookupSig:    string -> signatures option,
        lookupFunct:  string -> functors option,
        enterVal:     string * values      -> unit,
        enterType:    string * typeConstrSet -> unit,
        enterFix:     string * fixStatus   -> unit,
        enterStruct:  string * structVals  -> unit,
        enterSig:     string * signatures  -> unit,
        enterFunct:   string * functors    -> unit,
        allValNames:  unit -> string list
    }

    (* This creates functions for entering and looking up names. *)
    fun makeEnv tab =
        Env { lookupVal    = makeLook  valueVar      tab,
          lookupType   = makeLook  typeConstrVar tab,
          lookupFix    = makeLook  fixVar        tab,
          lookupStruct = makeLook  structVar     tab,
          lookupSig    = makeLook  signatureVar  tab,
          lookupFunct  = makeLook  functorVar    tab,
          enterVal     = makeEnter valueVar      tab,
          enterType    = makeEnter typeConstrVar tab,
          enterFix     = makeEnter fixVar        tab,
          enterStruct  = makeEnter structVar     tab,
          enterSig     = makeEnter signatureVar  tab,
          enterFunct   = makeEnter functorVar    tab,
          allValNames  = makeAllNames valueVar   tab
        }

    structure Sharing =
    struct
        type codetree   = codetree
        and  signatures = signatures
        and  types      = types
        and  values     = values
        and  typeId     = typeId
        and  structVals = structVals
        and  valAccess  = valAccess
        and  typeConstrs= typeConstrs
        and  typeConstrSet=typeConstrSet
        and  env        = env
        and  univTable  = univTable
        and  fixStatus  = fixStatus
        and  infixity   = infixity
        and  functors   = functors
        and  locationProp = locationProp
        and  typeVarForm = typeVarForm
        and  level      = level
    end
end (* STRUCTVALS *);
