/* 
 * TUI Calculator — Pro C89/C90 compatible (Dev-C++/old MinGW friendly)
 * 新增：变量/常量、/diff /solve /integ /plot、/hex /bin 等高级功能
 * 仍为 C89：不使用 for(int i=0;...)、避免 tgamma/round、全部类型加前缀防冲突
 *
 * Build:
 *   Windows(Dev-C++/MinGW): gcc tui_calc_pro_c89.c -O2 -o calc.exe -lm
 *   Linux/macOS:            gcc tui_calc_pro_c89.c -O2 -lm -o calc
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>

#ifdef _WIN32
#  include <windows.h>
#endif

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

/* snprintf 兼容 */
#ifdef _MSC_VER
#  if _MSC_VER < 1900
#    define snprintf _snprintf
#  endif
#endif

/* ------------ 配置 ------------ */
#define MAX_LINE     512
#define MAX_TOKENS   1024
#define MAX_STACK    1024
#define MAX_HISTORY  50
#define MAX_VARS     64
#define NAME_LEN     16

/* ------------ 平台 ------------ */
static void enable_ansi_if_windows(void){
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (hOut!=INVALID_HANDLE_VALUE && GetConsoleMode(hOut,&mode)){
        mode |= 0x0004; /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */
        SetConsoleMode(hOut,mode);
    }
#endif
}
static void clear_screen(void){ printf("\x1b[2J\x1b[H"); }

/* ------------ 角度模式 ------------ */
typedef enum { MODE_RAD=0, MODE_DEG=1 } AngleMode;
static AngleMode g_mode = MODE_RAD;
static double to_radian(double x){ return (g_mode==MODE_DEG)? x*M_PI/180.0 : x; }
static double from_radian(double x){ return (g_mode==MODE_DEG)? x*180.0/M_PI : x; }

/* ------------ 历史 ------------ */
typedef struct {
    char  *expr;
    double result;
    int    ok;
    char   err[128];
} HistoryItem;

static HistoryItem g_hist[MAX_HISTORY];
static int    g_hist_count  = 0;
static double g_last_result = 0.0;
static double g_memory      = 0.0;

static char* dupstr_local(const char* s){
    size_t n = strlen(s)+1;
    char* p = (char*)malloc(n);
    if(p) memcpy(p,s,n);
    return p;
}
static void history_add(const char* expr,double value,int ok,const char* errmsg){
    if(g_hist_count==MAX_HISTORY){
        int i;
        if(g_hist[0].expr) free(g_hist[0].expr);
        for(i=1;i<MAX_HISTORY;++i) g_hist[i-1]=g_hist[i];
        g_hist_count--;
    }
    g_hist[g_hist_count].expr = dupstr_local(expr);
    g_hist[g_hist_count].result=value;
    g_hist[g_hist_count].ok=ok;
    g_hist[g_hist_count].err[0]='\0';
    if(!ok && errmsg){
        strncpy(g_hist[g_hist_count].err,errmsg,sizeof(g_hist[g_hist_count].err)-1);
        g_hist[g_hist_count].err[sizeof(g_hist[g_hist_count].err)-1]='\0';
    }
    g_hist_count++;
}
static void history_print(void){
    int i;
    printf("History (newest last):\n");
    for(i=0;i<g_hist_count;++i){
        printf("  [%02d] %s  =>  ",i+1,g_hist[i].expr?g_hist[i].expr:"(null)");
        if(g_hist[i].ok) printf("%.15g\n",g_hist[i].result);
        else printf("ERROR: %s\n",g_hist[i].err);
    }
}
static int history_save(const char* file){
    int i;
    FILE* fp=fopen(file,"w");
    if(!fp) return -1;
    for(i=0;i<g_hist_count;++i){
        if(g_hist[i].ok) fprintf(fp,"[%02d] %s = %.15g\n",i+1,g_hist[i].expr,g_hist[i].result);
        else fprintf(fp,"[%02d] %s = ERROR(%s)\n",i+1,g_hist[i].expr,g_hist[i].err);
    }
    fclose(fp); return 0;
}

/* ------------ 变量表 ------------ */
typedef struct { char name[NAME_LEN]; double value; int in_use; } VarItem;
static VarItem g_vars[MAX_VARS];

static int var_find_index(const char* name){
    int i;
    for(i=0;i<MAX_VARS;++i)
        if(g_vars[i].in_use && strcmp(g_vars[i].name,name)==0) return i;
    return -1;
}
static int var_set(const char* name,double v){
    int i=var_find_index(name);
    if(i>=0){ g_vars[i].value=v; return 1; }
    for(i=0;i<MAX_VARS;++i){
        if(!g_vars[i].in_use){
            strncpy(g_vars[i].name,name,NAME_LEN-1);
            g_vars[i].name[NAME_LEN-1]='\0';
            g_vars[i].value=v;
            g_vars[i].in_use=1;
            return 1;
        }
    }
    return 0;
}
static int var_get(const char* name,double* out){
    int i=var_find_index(name);
    if(i>=0){ if(out) *out=g_vars[i].value; return 1; }
    return 0;
}
static int var_del(const char* name){
    int i=var_find_index(name);
    if(i>=0){ g_vars[i].in_use=0; g_vars[i].name[0]='\0'; return 1; }
    return 0;
}
static void var_list(void){
    int i, cnt=0;
    printf("Variables:\n");
    for(i=0;i<MAX_VARS;++i) if(g_vars[i].in_use){
        printf("  %-8s = %.15g\n", g_vars[i].name, g_vars[i].value);
        cnt++;
    }
    if(cnt==0) printf("  (none)\n");
}

/* 预置常量 */
static void vars_init_defaults(void){
    var_set("pi", M_PI);
    var_set("e", exp(1.0));
    var_set("ans", 0.0);
}

/* ------------ 词法/语法（前缀名防冲突） ------------ */
typedef enum {
    CALC_T_NUMBER, CALC_T_OPERATOR, CALC_T_LPAREN, CALC_T_RPAREN,
    CALC_T_FUNC, CALC_T_COMMA, CALC_T_IDENT /* 变量/常量名 */
} CalcTokType;

typedef enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_POW,
    OP_UNARY_MINUS, OP_FACT, OP_PERCENT
} OpKind;

typedef struct {
    CalcTokType type;
    double  value;
    OpKind  op;
    char    name[NAME_LEN]; /* 函数名或标识符名 */
    int     arity;          /* 函数元数 */
} CalcToken;

typedef struct { CalcToken items[MAX_TOKENS]; int count; } CalcTokenList;

static int is_func_char_local(int c){ return isalpha(c) || c=='_'; }
static int is_func_name_local(const char* s,int* ar){
    if(!s) return 0;
    if(strcmp(s,"sin")==0 || strcmp(s,"cos")==0 || strcmp(s,"tan")==0 ||
       strcmp(s,"sqrt")==0|| strcmp(s,"ln")==0  || strcmp(s,"log")==0 ||
       strcmp(s,"abs")==0 || strcmp(s,"exp")==0 ||
       strcmp(s,"asin")==0|| strcmp(s,"acos")==0|| strcmp(s,"atan")==0){
        if(ar) *ar=1; return 1;
    }
    if(strcmp(s,"pow")==0){ if(ar) *ar=2; return 1; }
    return 0;
}
static int precedence_local(OpKind op){
    switch(op){
        case OP_FACT: case OP_PERCENT: return 5;
        case OP_UNARY_MINUS: return 4;
        case OP_POW: return 3;
        case OP_MUL: case OP_DIV: return 2;
        case OP_ADD: case OP_SUB: return 1;
        default: return 0;
    }
}
static int is_right_assoc_local(OpKind op){ return (op==OP_POW || op==OP_UNARY_MINUS); }
static int is_postfix_local(OpKind op){ return (op==OP_FACT || op==OP_PERCENT); }

/* C89 版 round/近似整数 */
static double round_local(double x){ return (x>=0.0)? floor(x+0.5) : ceil(x-0.5); }
static int nearly_integer_local(double x){ return fabs(x-round_local(x)) < 1e-9; }

/* 阶乘（用 lgamma，C89 可用；若你环境无 lgamma，可改成小范围循环版） */
static int factorial_ok_local(double x){
    if(!nearly_integer_local(x)) return 0;
    if(x<0.0 || x>170.0) return 0;
    return 1;
}
static double factorial_val_local(double x){
    double n=round_local(x);
    return exp(lgamma(n+1.0));
}

/* 词法：数字/函数/变量/操作符/括号/逗号 */
static int tokenize_local(const char* s, CalcTokenList* out, char* errmsg, size_t emlen){
    size_t i=0, n=strlen(s);
    CalcTokType prev=CALC_T_OPERATOR;
    out->count=0;

    while(i<n){
        char c=s[i];

        if((unsigned char)c <= ' '){ i++; continue; }

        if((c>='0' && c<='9') || c=='.'){
            char* endp=NULL; double v;
            errno=0; v=strtod(s+i,&endp);
            if(s+i==endp){ snprintf(errmsg,emlen,"非法数字"); return 0; }
            if(errno==ERANGE){ snprintf(errmsg,emlen,"数字越界"); return 0; }
            out->items[out->count].type=CALC_T_NUMBER;
            out->items[out->count].value=v;
            out->count++;
            i=(size_t)(endp-s);
            prev=CALC_T_NUMBER;
            continue;
        }

        if(is_func_char_local((unsigned char)c)){
            char buf[NAME_LEN]; int j=0, ar=0, isf=0;
            while(i<n && is_func_char_local((unsigned char)s[i]) && j<NAME_LEN-1){
                buf[j++]=(char)tolower((unsigned char)s[i]); i++;
            }
            buf[j]='\0';
            isf = is_func_name_local(buf,&ar);
            if(isf){
                out->items[out->count].type=CALC_T_FUNC;
                strncpy(out->items[out->count].name,buf,NAME_LEN-1);
                out->items[out->count].name[NAME_LEN-1]='\0';
                out->items[out->count].arity=ar;
                out->count++;
                prev=CALC_T_FUNC;
            }else{
                /* 作为标识符（变量/常量） */
                out->items[out->count].type=CALC_T_IDENT;
                strncpy(out->items[out->count].name,buf,NAME_LEN-1);
                out->items[out->count].name[NAME_LEN-1]='\0';
                out->count++;
                prev=CALC_T_IDENT;
            }
            continue;
        }

        if(c=='('){ out->items[out->count].type=CALC_T_LPAREN; out->count++; i++; prev=CALC_T_LPAREN; continue; }
        if(c==')'){ out->items[out->count].type=CALC_T_RPAREN; out->count++; i++; prev=CALC_T_RPAREN; continue; }
        if(c==','){ out->items[out->count].type=CALC_T_COMMA;  out->count++; i++; prev=CALC_T_COMMA;  continue; }

        if(c=='+'||c=='-'||c=='*'||c=='/'||c=='^'||c=='!'||c=='%'){
            out->items[out->count].type=CALC_T_OPERATOR;
            if(c=='+') out->items[out->count].op=OP_ADD;
            else if(c=='-'){
                if(prev==CALC_T_OPERATOR||prev==CALC_T_LPAREN||prev==CALC_T_COMMA||out->count==0)
                    out->items[out->count].op=OP_UNARY_MINUS;
                else out->items[out->count].op=OP_SUB;
            } else if(c=='*') out->items[out->count].op=OP_MUL;
            else if(c=='/') out->items[out->count].op=OP_DIV;
            else if(c=='^') out->items[out->count].op=OP_POW;
            else if(c=='!') out->items[out->count].op=OP_FACT;
            else if(c=='%') out->items[out->count].op=OP_PERCENT;

            out->count++; i++; prev=CALC_T_OPERATOR; continue;
        }

        snprintf(errmsg,emlen,"无法识别的字符: '%c'",c);
        return 0;
    }
    return 1;
}

/* Shunting Yard：中缀->RPN */
static int to_rpn_local(const CalcTokenList* in, CalcTokenList* out, char* errmsg, size_t emlen){
    CalcToken opstack[MAX_STACK]; int top=0, i;
    out->count=0;
    for(i=0;i<in->count;++i){
        CalcToken tk=in->items[i];
        if(tk.type==CALC_T_NUMBER || tk.type==CALC_T_IDENT){
            out->items[out->count++]=tk;
        }else if(tk.type==CALC_T_FUNC){
            opstack[top++]=tk;
        }else if(tk.type==CALC_T_OPERATOR){
            while(top>0 && opstack[top-1].type==CALC_T_OPERATOR){
                OpKind o2=opstack[top-1].op;
                int p1=precedence_local(tk.op), p2=precedence_local(o2);
                if((!is_right_assoc_local(tk.op) && p1<=p2) ||
                   ( is_right_assoc_local(tk.op) && p1< p2)){
                    out->items[out->count++]=opstack[--top];
                }else break;
            }
            opstack[top++]=tk;
        }else if(tk.type==CALC_T_LPAREN){
            opstack[top++]=tk;
        }else if(tk.type==CALC_T_COMMA){
            int found=0;
            while(top>0){
                if(opstack[top-1].type==CALC_T_LPAREN){ found=1; break; }
                out->items[out->count++]=opstack[--top];
            }
            if(!found){ snprintf(errmsg,emlen,"逗号位置或括号不匹配"); return 0; }
        }else if(tk.type==CALC_T_RPAREN){
            while(top>0 && opstack[top-1].type!=CALC_T_LPAREN){
                out->items[out->count++]=opstack[--top];
            }
            if(top==0){ snprintf(errmsg,emlen,"括号不匹配"); return 0; }
            --top; /* pop '(' */
            if(top>0 && opstack[top-1].type==CALC_T_FUNC)
                out->items[out->count++]=opstack[--top];
        }
    }
    while(top>0){
        if(opstack[top-1].type==CALC_T_LPAREN || opstack[top-1].type==CALC_T_RPAREN){
            snprintf(errmsg,emlen,"括号不匹配"); return 0;
        }
        out->items[out->count++]=opstack[--top];
    }
    return 1;
}

/* 计算 RPN（变量在此处查表） */
static int eval_rpn_local(const CalcTokenList* rpn,double* outv,char* errmsg,size_t emlen){
    double st[MAX_STACK]; int sp=0, i;
    for(i=0;i<rpn->count;++i){
        CalcToken tk=rpn->items[i];
        if(tk.type==CALC_T_NUMBER){
            if(sp>=MAX_STACK){ snprintf(errmsg,emlen,"栈溢出"); return 0; }
            st[sp++]=tk.value;
        }else if(tk.type==CALC_T_IDENT){
            double v;
            if(strcmp(tk.name,"ans")==0) v=g_last_result;
            else if(!var_get(tk.name,&v)){ snprintf(errmsg,emlen,"未定义变量: %s",tk.name); return 0; }
            if(sp>=MAX_STACK){ snprintf(errmsg,emlen,"栈溢出"); return 0; }
            st[sp++]=v;
        }else if(tk.type==CALC_T_OPERATOR){
            if(is_postfix_local(tk.op)){
                double a;
                if(sp<1){ snprintf(errmsg,emlen,"缺少操作数"); return 0; }
                a=st[--sp];
                if(tk.op==OP_FACT){
                    if(!factorial_ok_local(a)){ snprintf(errmsg,emlen,"阶乘参数需为[0..170]整数"); return 0; }
                    st[sp++]=factorial_val_local(a);
                }else if(tk.op==OP_PERCENT){
                    st[sp++]=a*0.01;
                }
                continue;
            }
            if(tk.op==OP_UNARY_MINUS){
                if(sp<1){ snprintf(errmsg,emlen,"一元负号缺少操作数"); return 0; }
                st[sp-1]=-st[sp-1]; continue;
            }
            if(sp<2){ snprintf(errmsg,emlen,"二元操作缺少操作数"); return 0; }
            else{
                double b=st[--sp], a=st[--sp];
                switch(tk.op){
                    case OP_ADD: st[sp++]=a+b; break;
                    case OP_SUB: st[sp++]=a-b; break;
                    case OP_MUL: st[sp++]=a*b; break;
                    case OP_DIV:
                        if(b==0.0){ snprintf(errmsg,emlen,"除零错误"); return 0; }
                        st[sp++]=a/b; break;
                    case OP_POW:
                        errno=0; st[sp++]=pow(a,b);
                        if(errno==EDOM||errno==ERANGE){ snprintf(errmsg,emlen,"幂运算越界/域错误"); return 0; }
                        break;
                    default: snprintf(errmsg,emlen,"未知操作"); return 0;
                }
            }
        }else if(tk.type==CALC_T_FUNC){
            if(tk.arity==1){
                double x,y=0.0;
                if(sp<1){ snprintf(errmsg,emlen,"函数参数不足"); return 0; }
                x=st[--sp];
                if(strcmp(tk.name,"sin")==0) y=sin(to_radian(x));
                else if(strcmp(tk.name,"cos")==0) y=cos(to_radian(x));
                else if(strcmp(tk.name,"tan")==0) y=tan(to_radian(x));
                else if(strcmp(tk.name,"asin")==0) y=from_radian(asin(x));
                else if(strcmp(tk.name,"acos")==0) y=from_radian(acos(x));
                else if(strcmp(tk.name,"atan")==0) y=from_radian(atan(x));
                else if(strcmp(tk.name,"sqrt")==0){ if(x<0.0){ snprintf(errmsg,emlen,"sqrt 负数域错误"); return 0;} y=sqrt(x); }
                else if(strcmp(tk.name,"ln")==0){ if(x<=0.0){ snprintf(errmsg,emlen,"ln 非正数域错误"); return 0;} y=log(x); }
                else if(strcmp(tk.name,"log")==0){ if(x<=0.0){ snprintf(errmsg,emlen,"log10 非正数域错误"); return 0;} y=log10(x); }
                else if(strcmp(tk.name,"abs")==0) y=fabs(x);
                else if(strcmp(tk.name,"exp")==0) y=exp(x);
                else { snprintf(errmsg,emlen,"未知函数"); return 0; }
                st[sp++]=y;
            }else if(tk.arity==2){
                double a,b,y2;
                if(strcmp(tk.name,"pow")!=0){ snprintf(errmsg,emlen,"未知多参函数"); return 0; }
                if(sp<2){ snprintf(errmsg,emlen,"pow 需要2个参数"); return 0; }
                b=st[--sp]; a=st[--sp];
                errno=0; y2=pow(a,b);
                if(errno==EDOM||errno==ERANGE){ snprintf(errmsg,emlen,"pow 域/范围错误"); return 0; }
                st[sp++]=y2;
            }else{
                snprintf(errmsg,emlen,"函数元数不支持"); return 0;
            }
        }else{
            snprintf(errmsg,emlen,"RPN 非法 token"); return 0;
        }
    }
    if(sp!=1){ snprintf(errmsg,emlen,"表达式错误(栈剩余=%d)",sp); return 0; }
    *outv=st[0]; return 1;
}

static int eval_expr_local(const char* expr,double* outv,char* errmsg,size_t emlen){
    CalcTokenList tl,rpn;
    if(!tokenize_local(expr,&tl,errmsg,emlen)) return 0;
    if(!to_rpn_local(&tl,&rpn,errmsg,emlen)) return 0;
    if(!eval_rpn_local(&rpn,outv,errmsg,emlen)) return 0;
    return 1;
}

/* 在 var=val 的上下文里求 expr */
static int eval_with_var(const char* expr,const char* vname,double x,double* out,char* err,size_t emlen){
    double old=0.0; int existed=var_get(vname,&old), ok;
    var_set(vname,x);
    ok=eval_expr_local(expr,out,err,emlen);
    if(existed) var_set(vname,old); else var_del(vname);
    return ok;
}

/* ------------ UI ------------ */
static void render_panel(const char* last_msg){
    clear_screen();
    printf("┌─────────────────────────────── TUI Calculator Pro ───────────────────────────┐\n");
    printf("│ Angle: %-3s  | Memory: %-12.6g | Last(ans): %-14.8g                         │\n",
           (g_mode==MODE_DEG?"DEG":"RAD"), g_memory, g_last_result);
    printf("├──────────────────────────────────────────────────────────────────────────────┤\n");
    printf("│ 直接输入表达式并回车；'=' 重复上一次；变量：/let x=3.2、/vars、/del x           │\n");
    printf("│ 高级：/diff /solve /integ /plot     进制：/hex /bin     模式：/deg /rad         │\n");
    printf("│ 历史：/history /save <file>   内存：/mc /mr /m+ [v] /m- [v]   帮助：/help       │\n");
    printf("├──────────────────────────────────────────────────────────────────────────────┤\n");
    if(last_msg && last_msg[0]){
        printf("│ 提示 Hint: %-70.70s │\n", last_msg);
        printf("├──────────────────────────────────────────────────────────────────────────────┤\n");
    }
    printf("│ 示例： sin(30)+cos(60) [/deg] | pow(2,10) | 5!+20%% | 使用变量：/let x=1.2;    │\n");
    printf("│      /diff sin(x) x 0.5 1e-5  | /plot sin(x) x -3.14 3.14 70 20               │\n");
    printf("└──────────────────────────────────────────────────────────────────────────────┘\n");
}

/* ------------ 命令 ------------ */
static int is_cmd_local(const char* s,const char* cmd){ return strcmp(s,cmd)==0; }
static void trim_spaces(char* s){
    int i=0,j=(int)strlen(s)-1;
    while(s[i] && (s[i]==' '||s[i]=='\t')) i++;
    while(j>=i && (s[j]==' '||s[j]=='\t')) j--;
    if(i>0) memmove(s,s+i,(size_t)(j-i+1));
    s[j-i+1]='\0';
}

/* 数值工具 */
static double diff_center(const char* expr,const char* v,double x,double h,char* er,size_t em){
    double f1,f2;
    if(!eval_with_var(expr,v,x+h, &f1,er,em)) return NAN;
    if(!eval_with_var(expr,v,x-h, &f2,er,em)) return NAN;
    return (f1-f2)/(2*h);
}
static int solve_newton(const char* expr,const char* v,double x0,int maxit,double tol,double* root,char* er,size_t em){
    int k;
    double x=x0;
    for(k=0;k<maxit;++k){
        double fx, dfx;
        if(!eval_with_var(expr,v,x,&fx,er,em)) return 0;
        dfx = diff_center(expr,v,x,1e-6,er,em);
        if(!isfinite(dfx) || dfx==0.0){ snprintf(er,em,"导数为0/非数 at x=%.15g",x); return 0; }
        x = x - fx/dfx;
        if(fabs(fx) < tol){ *root=x; return 1; }
    }
    snprintf(er,em,"迭代未收敛(maxit=%d)",maxit);
    return 0;
}
static int integ_simpson(const char* expr,const char* v,double a,double b,int n,double* out,char* er,size_t em){
    int i;
    double h, s=0.0, x, fx;
    if(n<=0) n=200;
    if(n%2) n++; /* Simpson 需要偶数段 */
    h=(b-a)/n;
    if(!eval_with_var(expr,v,a,&fx,er,em)) return 0; s+=fx;
    for(i=1;i<n;i++){
        x=a+i*h;
        if(!eval_with_var(expr,v,x,&fx,er,em)) return 0;
        s += (i%2 ? 4.0*fx : 2.0*fx);
    }
    if(!eval_with_var(expr,v,b,&fx,er,em)) return 0; s+=fx;
    *out = s*h/3.0; return 1;
}

/* ASCII plot */
static void plot_ascii(const char* expr,const char* v,double xmin,double xmax,int W,int H){
    int i,j;
    if(W<=0) W=60; if(W>120) W=120;
    if(H<=0) H=20; if(H>40)  H=40;

    /* 采样找 ymin/ymax */
    double ymin=1e300,ymax=-1e300, x, y;
    char err[128];
    for(i=0;i<W;++i){
        x = xmin + (xmax-xmin)*i/(W-1.0);
        if(!eval_with_var(expr,v,x,&y,err,sizeof(err))) continue;
        if(isfinite(y)){ if(y<ymin) ymin=y; if(y>ymax) ymax=y; }
    }
    if(!(isfinite(ymin)&&isfinite(ymax)) || ymin==ymax){ ymin-=1; ymax+=1; }

    /* 画布 */
    {
        char* grid = (char*)malloc((size_t)(W*H));
        for(i=0;i<H;++i) for(j=0;j<W;++j) grid[i*W+j]=' ';
        /* 坐标轴：x=0,y=0 */
        if(xmin<=0 && xmax>=0){
            int col = (int)((0 - xmin)/(xmax-xmin)*(W-1));
            if(col<0) col=0; if(col>=W) col=W-1;
            for(i=0;i<H;++i) grid[i*W+col]='|';
        }
        if(ymin<=0 && ymax>=0){
            int row = (int)((ymax - 0)/(ymax-ymin)*(H-1));
            if(row<0) row=0; if(row>=H) row=H-1;
            for(j=0;j<W;++j) grid[row*W+j]='-';
        }
        /* 曲线 */
        for(j=0;j<W;++j){
            x = xmin + (xmax-xmin)*j/(W-1.0);
            if(eval_with_var(expr,v,x,&y,err,sizeof(err)) && isfinite(y)){
                int row = (int)((ymax - y)/(ymax-ymin)*(H-1));
                if(row>=0 && row<H) grid[row*W+j]='*';
            }
        }
        /* 输出 */
        printf("\n y in [%.6g, %.6g]  x in [%.6g, %.6g]\n",ymin,ymax,xmin,xmax);
        for(i=0;i<H;++i){
            putchar(' ');
            for(j=0;j<W;++j) putchar(grid[i*W+j]);
            putchar('\n');
        }
        free(grid);
    }
}

/* 进制输出 */
static void print_bin(unsigned long v){
    int i, started=0;
    for(i=(int)(sizeof(unsigned long)*8-1); i>=0; --i){
        int bit = (v>>i)&1U;
        if(bit) started=1;
        if(started) putchar(bit?'1':'0');
    }
    if(!started) putchar('0');
}

/* 命令处理：返回 1 表示已处理 */
static int handle_command_local(char* line,char* msg,size_t msglen){
    char *cmd,*arg;

    if(line[0] != '/') return 0;
    cmd = strtok(line," \t\r\n");
    arg = strtok(NULL,"");

    if(is_cmd_local(cmd,"/help")){
        snprintf(msg,msglen,"命令: /deg /rad /mc /mr /m+ [v] /m- [v] /history /save f /let x=expr /vars /del x /diff e v x0 [h] /solve e v x0 [maxit tol] /integ e v a b [n] /plot e v xmin xmax [w h] /hex n /bin n /quit");
        return 1;
    }
    if(is_cmd_local(cmd,"/deg")){ g_mode=MODE_DEG; snprintf(msg,msglen,"已切换到 DEG"); return 1; }
    if(is_cmd_local(cmd,"/rad")){ g_mode=MODE_RAD; snprintf(msg,msglen,"已切换到 RAD"); return 1; }

    if(is_cmd_local(cmd,"/mc")){ g_memory=0.0; snprintf(msg,msglen,"Memory cleared"); return 1; }
    if(is_cmd_local(cmd,"/mr")){ snprintf(msg,msglen,"MR = %.15g",g_memory); g_last_result=g_memory; var_set("ans",g_last_result); return 1; }
    if(is_cmd_local(cmd,"/m+")){
        double v=g_last_result; if(arg) v=atof(arg);
        g_memory += v; snprintf(msg,msglen,"M += %.15g -> %.15g",v,g_memory); return 1;
    }
    if(is_cmd_local(cmd,"/m-")){
        double v=g_last_result; if(arg) v=atof(arg);
        g_memory -= v; snprintf(msg,msglen,"M -= %.15g -> %.15g",v,g_memory); return 1;
    }
    if(is_cmd_local(cmd,"/history")){
        clear_screen(); history_print(); printf("\n按回车返回..."); getchar();
        msg[0]='\0'; return 1;
    }
    if(is_cmd_local(cmd,"/save")){
        char* f = arg; if(!f || !*f){ snprintf(msg,msglen,"用法: /save <file>"); return 1; }
        trim_spaces(f);
        if(history_save(f)==0) snprintf(msg,msglen,"历史已保存: %s",f);
        else snprintf(msg,msglen,"保存失败");
        return 1;
    }
    if(is_cmd_local(cmd,"/vars")){ clear_screen(); var_list(); printf("\n按回车返回..."); getchar(); msg[0]='\0'; return 1; }
    if(is_cmd_local(cmd,"/del")){
        char* name = arg; if(!name){ snprintf(msg,msglen,"用法: /del <name>"); return 1; }
        trim_spaces(name); if(var_del(name)) snprintf(msg,msglen,"已删除变量: %s",name); else snprintf(msg,msglen,"不存在变量: %s",name);
        return 1;
    }
    if(is_cmd_local(cmd,"/let")){
        /* 支持 "/let x= expr" 或 "/let x expr" */
        char *p=arg,*eq; char name[NAME_LEN], rhs[MAX_LINE]; char err[128]; double val;
        if(!p){ snprintf(msg,msglen,"用法: /let <name>=<expr> 或 /let <name> <expr>"); return 1; }
        trim_spaces(p);
        eq=strchr(p,'=');
        if(eq){
            size_t L;
            *eq='\0'; trim_spaces(p); trim_spaces(eq+1);
            L=strlen(p); if(L==0||L>=NAME_LEN){ snprintf(msg,msglen,"变量名非法"); return 1; }
            strncpy(name,p,NAME_LEN-1); name[NAME_LEN-1]='\0';
            strncpy(rhs,eq+1,sizeof(rhs)-1); rhs[sizeof(rhs)-1]='\0';
        }else{
            /* 第一个 token 为 name，余下为 expr */
            char* sp=strpbrk(p," \t");
            if(!sp){ snprintf(msg,msglen,"用法: /let <name> <expr>"); return 1; }
            *sp='\0'; strncpy(name,p,NAME_LEN-1); name[NAME_LEN-1]='\0';
            strncpy(rhs,sp+1,sizeof(rhs)-1); rhs[sizeof(rhs)-1]='\0';
            trim_spaces(rhs);
        }
        if(!eval_expr_local(rhs,&val,err,sizeof(err))){
            snprintf(msg,msglen,"赋值失败: %s",err); return 1;
        }
        var_set(name,val); if(strcmp(name,"ans")==0) g_last_result=val;
        snprintf(msg,msglen,"%s = %.15g",name,val); return 1;
    }

    if(is_cmd_local(cmd,"/diff")){
        /* /diff <expr> <var> <x0> [h] */
        char e[MAX_LINE], vname[NAME_LEN]; double x0,h=1e-5; char* t;
        if(!arg){ snprintf(msg,msglen,"用法: /diff <expr> <var> <x0> [h]"); return 1; }
        /* 拆 expr（到下一个空白前的 token 不能含空格；支持用括号或无空格表达式——简单实现） */
        t=strtok(arg," \t\r\n"); if(!t){ snprintf(msg,msglen,"参数不足"); return 1; }
        strncpy(e,t,sizeof(e)-1); e[sizeof(e)-1]='\0';
        t=strtok(NULL," \t\r\n"); if(!t){ snprintf(msg,msglen,"缺少 <var>"); return 1; }
        strncpy(vname,t,NAME_LEN-1); vname[NAME_LEN-1]='\0';
        t=strtok(NULL," \t\r\n"); if(!t){ snprintf(msg,msglen,"缺少 <x0>"); return 1; }
        x0=atof(t);
        t=strtok(NULL," \t\r\n"); if(t) h=atof(t);
        {
            char er[128]; double d = diff_center(e,vname,x0,h,er,sizeof(er));
            if(!isfinite(d)){ snprintf(msg,msglen,"/diff 失败: %s",er); }
            else snprintf(msg,msglen,"d/d%s %s | x=%.6g ≈ %.15g (h=%.1e)",vname,e,x0,d,h);
        }
        return 1;
    }

    if(is_cmd_local(cmd,"/solve")){
        /* /solve <expr> <var> <x0> [maxit tol] */
        char e[MAX_LINE], vname[NAME_LEN], *t; double x0; int maxit=30; double tol=1e-10;
        if(!arg){ snprintf(msg,msglen,"用法: /solve <expr> <var> <x0> [maxit tol]"); return 1; }
        t=strtok(arg," \t\r\n"); if(!t){ snprintf(msg,msglen,"参数不足"); return 1; }
        strncpy(e,t,sizeof(e)-1); e[sizeof(e)-1]='\0';
        t=strtok(NULL," \t\r\n"); if(!t){ snprintf(msg,msglen,"缺少 <var>"); return 1; }
        strncpy(vname,t,NAME_LEN-1); vname[NAME_LEN-1]='\0';
        t=strtok(NULL," \t\r\n"); if(!t){ snprintf(msg,msglen,"缺少 <x0>"); return 1; }
        x0=atof(t);
        t=strtok(NULL," \t\r\n"); if(t) { maxit=atoi(t); t=strtok(NULL," \t\r\n"); if(t) tol=atof(t); }
        {
            char er[128]; double r;
            if(solve_newton(e,vname,x0,maxit,tol,&r,er,sizeof(er))){ snprintf(msg,msglen,"root≈ %.15g",r); }
            else snprintf(msg,msglen,"/solve 失败: %s",er);
        }
        return 1;
    }

    if(is_cmd_local(cmd,"/integ")){
        /* /integ <expr> <var> <a> <b> [n] */
        char e[MAX_LINE], vname[NAME_LEN], *t; double a,b; int n=200; char er[128]; double val;
        if(!arg){ snprintf(msg,msglen,"用法: /integ <expr> <var> <a> <b> [n]"); return 1; }
        t=strtok(arg," \t\r\n"); if(!t){ snprintf(msg,msglen,"参数不足"); return 1; }
        strncpy(e,t,sizeof(e)-1); e[sizeof(e)-1]='\0';
        t=strtok(NULL," \t\r\n"); if(!t){ snprintf(msg,msglen,"缺少 <var>"); return 1; }
        strncpy(vname,t,NAME_LEN-1); vname[NAME_LEN-1]='\0';
        t=strtok(NULL," \t\r\n"); if(!t){ snprintf(msg,msglen,"缺少 <a>"); return 1; }
        a=atof(t);
        t=strtok(NULL," \t\r\n"); if(!t){ snprintf(msg,msglen,"缺少 <b>"); return 1; }
        b=atof(t);
        t=strtok(NULL," \t\r\n"); if(t) n=atoi(t);
        if(integ_simpson(e,vname,a,b,n,&val,er,sizeof(er))) snprintf(msg,msglen,"∫[%g,%g] %s d%s ≈ %.15g (n=%d)",a,b,e,vname,val,n);
        else snprintf(msg,msglen,"/integ 失败: %s",er);
        return 1;
    }

    if(is_cmd_local(cmd,"/plot")){
        /* /plot <expr> <var> <xmin> <xmax> [W H] */
        char e[MAX_LINE], vname[NAME_LEN], *t; double xmin,xmax; int W=60,H=20;
        if(!arg){ snprintf(msg,msglen,"用法: /plot <expr> <var> <xmin> <xmax> [W H]"); return 1; }
        t=strtok(arg," \t\r\n"); if(!t){ snprintf(msg,msglen,"参数不足"); return 1; }
        strncpy(e,t,sizeof(e)-1); e[sizeof(e)-1]='\0';
        t=strtok(NULL," \t\r\n"); if(!t){ snprintf(msg,msglen,"缺少 <var>"); return 1; }
        strncpy(vname,t,NAME_LEN-1); vname[NAME_LEN-1]='\0';
        t=strtok(NULL," \t\r\n"); if(!t){ snprintf(msg,msglen,"缺少 <xmin>"); return 1; }
        xmin=atof(t);
        t=strtok(NULL," \t\r\n"); if(!t){ snprintf(msg,msglen,"缺少 <xmax>"); return 1; }
        xmax=atof(t);
        t=strtok(NULL," \t\r\n"); if(t){ W=atoi(t); t=strtok(NULL," \t\r\n"); if(t) H=atoi(t); }
        plot_ascii(e,vname,xmin,xmax,W,H);
        snprintf(msg,msglen,"已绘图：%s, %s∈[%.6g,%.6g], %dx%d",e,vname,xmin,xmax,W,H);
        return 1;
    }

    if(is_cmd_local(cmd,"/hex")){
        unsigned long v = arg ? strtoul(arg,NULL,10) : 0UL;
        printf("\n0x%lX\n", v); msg[0]='\0'; return 1;
    }
    if(is_cmd_local(cmd,"/bin")){
        unsigned long v = arg ? strtoul(arg,NULL,10) : 0UL;
        printf("\n"); print_bin(v); printf("\n"); msg[0]='\0'; return 1;
    }

    if(is_cmd_local(cmd,"/quit")){ exit(0); }

    snprintf(msg,msglen,"未知命令: %s (/help 查看)",cmd);
    return 1;
}

/* ------------ 自检（简要） ------------ */
typedef struct { const char* expr; double expect; double tol; } CaseItem;
static int run_selftest_local(void){
    int pass=0,total=0,i;
    CaseItem c1[]={
        {"1+2*3",7,1e-12},{"(2+3)*4",20,1e-12},{"-3^2",-9,1e-12},{"(-3)^2",9,1e-12},
        {"5!",120,1e-12},{"50%",0.5,1e-12},{"sqrt(2)^2",2,1e-12},{"ln(exp(1))",1,1e-12},
        {"log(1000)",3,1e-12},{"pow(2,10)",1024,1e-12},{NULL,0,0}
    };
    char err[128]; double out;
    g_mode=MODE_RAD;
    for(i=0;c1[i].expr;++i){ total++; if(eval_expr_local(c1[i].expr,&out,err,sizeof(err)) && fabs(out-c1[i].expect)<=c1[i].tol) pass++; }
    printf("SelfTest basic: %d/%d\n",pass,total);
    return (pass==total)?0:1;
}

/* ------------ 主循环 ------------ */
int main(int argc,char** argv){
    char line[MAX_LINE], msg[160]="输入表达式或用 /help 查看进阶命令", last_expr[MAX_LINE];
    int len;

    enable_ansi_if_windows();
    vars_init_defaults();

    if(argc>1 && strcmp(argv[1],"--selftest")==0) return run_selftest_local();

    last_expr[0]='\0';

    for(;;){
        render_panel(msg);
        printf("\n> 请输入表达式或命令: ");
        if(!fgets(line,sizeof(line),stdin)) break;
        len=(int)strlen(line);
        while(len>0 && (line[len-1]=='\n'||line[len-1]=='\r')) line[--len]=0;
        if(len==0){ msg[0]='\0'; continue; }

        if(strcmp(line,"=")==0){
            if(last_expr[0]==0){ snprintf(msg,sizeof(msg),"无上次表达式可重复"); continue; }
            strncpy(line,last_expr,sizeof(line)-1); line[sizeof(line)-1]='\0';
        }

        {
            char work[MAX_LINE];
            strncpy(work,line,sizeof(work)-1); work[sizeof(work)-1]='\0';
            if(handle_command_local(work,msg,sizeof(msg))) continue;
        }

        {
            double val=0.0; char err[128]; err[0]='\0';
            if(eval_expr_local(line,&val,err,sizeof(err))){
                g_last_result=val; var_set("ans",g_last_result);
                snprintf(msg,sizeof(msg),"结果 = %.15g",val);
                strncpy(last_expr,line,sizeof(last_expr)-1); last_expr[sizeof(last_expr)-1]='\0';
                history_add(line,val,1,NULL);
            }else{
                snprintf(msg,sizeof(msg),"错误: %s",err);
                history_add(line,0.0,0,err);
            }
        }
    }
    return 0;
}

