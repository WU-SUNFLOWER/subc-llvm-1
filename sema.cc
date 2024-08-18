#include "sema.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Casting.h"

std::shared_ptr<AstNode> Sema::SemaVariableDeclNode(Token tok, std::shared_ptr<CType> ty, bool isGlobal) {
    // 1. 检测是否出现重定义
    llvm::StringRef text(tok.ptr, tok.len);
    std::shared_ptr<Symbol> symbol = scope.FindObjSymbolInCurEnv(text);
    if (symbol) {
        diagEngine.Report(llvm::SMLoc::getFromPointer(tok.ptr), diag::err_redefined, text);
    }
    if (mode == Mode::Normal) {
        /// 2. 添加到符号表
        scope.AddObjSymbol(ty, text);
    }

    /// 3. 返回结点
    auto decl = std::make_shared<VariableDecl>();
    decl->tok = tok;
    decl->ty = ty;
    decl->isLValue = true;
    decl->isGlobal = isGlobal;
    return decl;
}

std::shared_ptr<AstNode> Sema::SemaVariableAccessNode(Token tok)  {

    llvm::StringRef text(tok.ptr, tok.len);
    std::shared_ptr<Symbol> symbol = scope.FindObjSymbol(text);
    if (symbol == nullptr && (mode == Mode::Normal)) {
        diagEngine.Report(llvm::SMLoc::getFromPointer(tok.ptr), diag::err_undefined, text);
    }

    auto expr = std::make_shared<VariableAccessExpr>();
    expr->tok = tok;
    expr->ty = symbol->GetTy();
    expr->isLValue = true;
    return expr;
}

std::shared_ptr<AstNode> Sema::SemaBinaryExprNode( std::shared_ptr<AstNode> left,std::shared_ptr<AstNode> right, BinaryOp op) {
    auto binaryExpr = std::make_shared<BinaryExpr>();
    binaryExpr->op = op;
    binaryExpr->left = left;
    binaryExpr->right = right;
    binaryExpr->ty = left->ty;

    if (op == BinaryOp::add || op == BinaryOp::sub || op == BinaryOp::add_assign || op == BinaryOp::sub_assign) {
        /// int a = 3; int *p = &a; 3+p;
        if ((left->ty->GetKind() == CType::TY_Int) && (right->ty->GetKind() == CType::TY_Point)) {
            binaryExpr->ty = right->ty;
        }
    }
    return binaryExpr;
}

std::shared_ptr<AstNode> Sema::SemaUnaryExprNode( std::shared_ptr<AstNode> unary, UnaryOp op, Token tok) {
    auto node = std::make_shared<UnaryExpr>();
    node->op = op;
    node->node = unary;

    switch (op)
    {
    case UnaryOp::positive:
    case UnaryOp::negative:
    case UnaryOp::logical_not:
    case UnaryOp::bitwise_not:
    {
        if (unary->ty->GetKind() != CType::TY_Int && (mode == Mode::Normal)) {
            diagEngine.Report(llvm::SMLoc::getFromPointer(tok.ptr), diag::err_expected_ype, "int type");
        }
        node->ty = unary->ty;
        break;
    }
    case UnaryOp::addr: {
        /// &a; 
        // if (!unary->isLValue) {
        //     diagEngine.Report(llvm::SMLoc::getFromPointer(tok.ptr), diag::err_expected_lvalue);
        // }
        node->ty = std::make_shared<CPointType>(unary->ty);
        break;
    }
    case UnaryOp::deref: {
        /// *a;
        /// 语义判断 must be pointer
        if (unary->ty->GetKind() != CType::TY_Point && (mode == Mode::Normal)) {
            diagEngine.Report(llvm::SMLoc::getFromPointer(tok.ptr), diag::err_expected_ype, "pointer type");
        }
        // if (!unary->isLValue) {
        //     diagEngine.Report(llvm::SMLoc::getFromPointer(tok.ptr), diag::err_expected_lvalue);
        // }
        CPointType *pty = llvm::dyn_cast<CPointType>(unary->ty.get());
        node->ty = pty->GetBaseType();
        node->isLValue = true;
        break;   
    }
    case UnaryOp::dec:
    case UnaryOp::inc: {
        if (!unary->isLValue) {
            diagEngine.Report(llvm::SMLoc::getFromPointer(tok.ptr), diag::err_expected_lvalue);
        }
        node->ty = unary->ty;
        break;          
    }                       
    default:
        break;
    }

    return node;
}

std::shared_ptr<AstNode> Sema::SemaThreeExprNode( std::shared_ptr<AstNode> cond,std::shared_ptr<AstNode> then, std::shared_ptr<AstNode> els, Token tok) {
    auto node = std::make_shared<ThreeExpr>();
    node->cond = cond;
    node->then = then;
    node->els = els;
    if (then->ty->GetKind() != els->ty->GetKind() && (mode == Mode::Normal)) {
        diagEngine.Report(llvm::SMLoc::getFromPointer(tok.ptr), diag::err_same_type);
    }
    node->ty = then->ty;
    return node;
}

// sizeof a;
std::shared_ptr<AstNode> Sema::SemaSizeofExprNode( std::shared_ptr<AstNode> unary,std::shared_ptr<CType> ty) {
    auto node = std::make_shared<SizeOfExpr>();
    node->type = ty;
    node->node = unary;
    node->ty = CType::IntType;
    return node;
}

std::shared_ptr<AstNode> Sema::SemaPostIncExprNode(std::shared_ptr<AstNode> left, Token tok) {
    if (!left->isLValue && (mode == Mode::Normal)) {
        diagEngine.Report(llvm::SMLoc::getFromPointer(tok.ptr), diag::err_expected_lvalue);
    }
    auto node = std::make_shared<PostIncExpr>();
    node->left = left;
    node->ty = left->ty;
    return node;
}

/// a--
std::shared_ptr<AstNode> Sema::SemaPostDecExprNode( std::shared_ptr<AstNode> left, Token tok) {
    if (!left->isLValue && (mode == Mode::Normal)) {
        diagEngine.Report(llvm::SMLoc::getFromPointer(tok.ptr), diag::err_expected_lvalue);
    }
    auto node = std::make_shared<PostDecExpr>();
    node->left = left;
    node->ty = left->ty;
    return node;
}
/// a[1]; -> *(a + offset(1 * elementSize));
std::shared_ptr<AstNode> Sema::SemaPostSubscriptNode(std::shared_ptr<AstNode> left, std::shared_ptr<AstNode> node, Token tok) {
    if (left->ty->GetKind() != CType::TY_Array && left->ty->GetKind() != CType::TY_Point && (mode == Mode::Normal)) {
        diagEngine.Report(llvm::SMLoc::getFromPointer(tok.ptr), diag::err_expected_ype, "array or point");
    }
    auto postSubScriptNode = std::make_shared<PostSubscript>();
    postSubScriptNode->left = left;
    postSubScriptNode->node = node;
    if (left->ty->GetKind() == CType::TY_Array) {
        CArrayType *arrType = llvm::dyn_cast<CArrayType>(left->ty.get());
        postSubScriptNode->ty = arrType->GetElementType();
    }else if (left->ty->GetKind() == CType::TY_Point) {
        CPointType *pointType = llvm::dyn_cast<CPointType>(left->ty.get());
        postSubScriptNode->ty = pointType->GetBaseType();
    }
    return postSubScriptNode;
}

std::shared_ptr<AstNode> Sema::SemaPostMemberDotNode(std::shared_ptr<AstNode> left, Token iden, Token dotTok) {
    if (left->ty->GetKind() != CType::TY_Record) {
        diagEngine.Report(llvm::SMLoc::getFromPointer(dotTok.ptr), diag::err_expected_ype, "struct or union type");
    }

    CRecordType *recordType = llvm::dyn_cast<CRecordType>(left->ty.get());
    const auto &members = recordType->GetMembers();

    bool found = false;
    Member curMember;
    for (const auto &member : members) {
        if (member.name == llvm::StringRef(iden.ptr, iden.len)) {
            found = true;
            curMember = member;
            break;
        }
    }
    if (!found) {
        diagEngine.Report(llvm::SMLoc::getFromPointer(iden.ptr), diag::err_miss, "struct or union miss field");
    }

    
    auto node = std::make_shared<PostMemberDotExpr>();
    node->tok = dotTok;
    node->ty = curMember.ty;
    node->left = left;
    node->member = curMember;
    return node;
}

std::shared_ptr<AstNode> Sema::SemaPostMemberArrowNode(std::shared_ptr<AstNode> left, Token iden, Token arrowTok) {
    if (left->ty->GetKind() != CType::TY_Point) {
        diagEngine.Report(llvm::SMLoc::getFromPointer(arrowTok.ptr), diag::err_expected_ype, "pointer type");
    }

    CPointType *pRecordType = llvm::dyn_cast<CPointType>(left->ty.get());
    if (pRecordType->GetBaseType()->GetKind() != CType::TY_Record) {
        diagEngine.Report(llvm::SMLoc::getFromPointer(arrowTok.ptr), diag::err_expected_ype, "pointer to struct or union type");
    }

    CRecordType *recordType = llvm::dyn_cast<CRecordType>(pRecordType->GetBaseType().get());
    const auto &members = recordType->GetMembers();

    bool found = false;
    Member curMember;
    for (const auto &member : members) {
        if (member.name == llvm::StringRef(iden.ptr, iden.len)) {
            found = true;
            curMember = member;
            break;
        }
    }
    if (!found) {
        diagEngine.Report(llvm::SMLoc::getFromPointer(iden.ptr), diag::err_miss, "struct or union miss field");
    }

    
    auto node = std::make_shared<PostMemberArrowExpr>();
    node->tok = arrowTok;
    node->ty = curMember.ty;
    node->left = left;
    node->member = curMember;
    return node;
}

std::shared_ptr<AstNode> Sema::SemaNumberExprNode(Token tok, std::shared_ptr<CType> ty) {
    auto expr = std::make_shared<NumberExpr>();
    expr->tok = tok;
    expr->ty = ty;
    return expr;
}
std::shared_ptr<VariableDecl::InitValue> Sema::SemaDeclInitValue(std::shared_ptr<CType> declType, std::shared_ptr<AstNode> value, std::vector<int> &offsetList, Token tok)
 {
    if (declType->GetKind() != value->ty->GetKind() && (mode == Mode::Normal)) {
        diagEngine.Report(llvm::SMLoc::getFromPointer(tok.ptr), diag::err_miss, "same type");
    }
    auto initValue = std::make_shared<VariableDecl::InitValue>();
    initValue->declType = declType;
    initValue->value = value;
    initValue->offsetList = offsetList;
    return initValue;
 }

std::shared_ptr<AstNode> Sema::SemaIfStmtNode(std::shared_ptr<AstNode> condNode, std::shared_ptr<AstNode> thenNode, std::shared_ptr<AstNode> elseNode) {
    auto node = std::make_shared<IfStmt>();
    node->condNode = condNode;
    node->thenNode = thenNode;
    node->elseNode = elseNode;
    return node;
}

std::shared_ptr<CType> Sema::SemaTagAccess(Token tok) {
    llvm::StringRef text(tok.ptr, tok.len);
    std::shared_ptr<Symbol> symbol = scope.FindTagSymbol(text);
    if (symbol == nullptr && (mode == Mode::Normal)) {
        diagEngine.Report(llvm::SMLoc::getFromPointer(tok.ptr), diag::err_undefined, text);
    }
    return symbol->GetTy();
}

std::shared_ptr<CType> Sema::SemaTagDecl(Token tok, const std::vector<Member> &members, TagKind tagKind) {
    // 1. 检测是否出现重定义
    llvm::StringRef text(tok.ptr, tok.len);
    std::shared_ptr<Symbol> symbol = scope.FindTagSymbolInCurEnv(text);
    if (symbol) {
        diagEngine.Report(llvm::SMLoc::getFromPointer(tok.ptr), diag::err_redefined, text);
    }
    auto recordTy = std::make_shared<CRecordType>(text, members, tagKind);
    if (mode == Mode::Normal) {
        /// 2. 添加到符号表
        scope.AddTagSymbol(recordTy, text);
    }
    return recordTy;
}

std::shared_ptr<CType> Sema::SemaAnonyTagDecl(const std::vector<Member> &members, TagKind tagKind) {
    llvm::StringRef text = CType::GenAnonyRecordName(tagKind);
    auto recordTy = std::make_shared<CRecordType>(text, members, tagKind);
    if (mode == Mode::Normal) {
        /// 2. 添加到符号表
        scope.AddTagSymbol(recordTy, text);
    }
    return recordTy;
}

std::shared_ptr<AstNode> Sema::SemaFuncDecl(Token tok, std::shared_ptr<CType> type, std::shared_ptr<AstNode> blockStmt) {
     // 1. 检测是否出现重定义
    llvm::StringRef text(tok.ptr, tok.len);
    std::shared_ptr<Symbol> symbol = scope.FindObjSymbolInCurEnv(text);
    if (symbol && blockStmt && (mode == Mode::Normal)) {
        diagEngine.Report(llvm::SMLoc::getFromPointer(tok.ptr), diag::err_redefined, text);
    }

    if (symbol == nullptr && (mode == Mode::Normal)) {
        /// 2. 添加到符号表
        scope.AddObjSymbol(type, text);
    }

    auto funcDecl = std::make_shared<FuncDecl>();
    funcDecl->ty = type;
    funcDecl->blockStmt = blockStmt;
    funcDecl->tok = tok;
    return funcDecl;
}

std::shared_ptr<AstNode> Sema::SemaFuncCall(std::shared_ptr<AstNode> left, const std::vector<std::shared_ptr<AstNode>> &args) {
    Token iden = left->tok;
    if (left->ty->GetKind() != CType::TY_Func) {
        diagEngine.Report(llvm::SMLoc::getFromPointer(iden.ptr), diag::err_expected, "functype");
    }

    CFuncType *funcType = llvm::dyn_cast<CFuncType>(left->ty.get());
    if (funcType->GetParams().size() != args.size()) {
        diagEngine.Report(llvm::SMLoc::getFromPointer(iden.ptr), diag::err_miss, "arg count not match");
    }

    auto funcCall = std::make_shared<PostFuncCall>();
    funcCall->ty = funcType->GetRetType();
    funcCall->left = left;
    funcCall->args = args;
    funcCall->tok = left->tok;
    return funcCall;
}

void Sema::EnterScope() {
    scope.EnterScope();
}

void Sema::ExitScope() {
    scope.ExitScope();
}

void Sema::SetMode(Mode mode) {
    this->mode = mode;
}