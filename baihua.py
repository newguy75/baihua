#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
白话：一门迷你中文编程语言
单文件解释器，没有第三方依赖，需要 Python 3.8 或更新的版本。

用法：
    python3 baihua.py demo.bh             运行一个程序文件（后面还可以跟参数，程序里用 命令行参数（） 取）
    python3 baihua.py                     进入交互模式
    python3 baihua.py --tokens demo.bh    只做词法分析，看每个符号被认成了什么

解释器的三个步骤（下面的代码也按这个顺序排）：
    源代码 -> [1 词法分析] -> 记号 -> [2 语法分析] -> 语法树 -> [3 解释执行] -> 结果

想改造这门语言，最常动的是这几处：
    KEYWORDS / WORD_OPERATORS    关键字和文字运算符（想换个说法就改这里）
    PUNCTUATION / QUOTES         认哪些中文标点
    BUILTINS                     内置函数（想加新函数就在这里加一行）
"""

import os
import random
import subprocess
import sys
from collections import namedtuple

VERSION = '0.3'


class BaihuaError(Exception):
    """白话程序里的错误。带着行号，最后用中文报给写程序的人。"""

    def __init__(self, message, line=None, kind='运行错误'):
        super().__init__(message)
        self.message = message
        self.line = line
        self.kind = kind
        self.frames = []       # 出错时正在进行的函数调用：[(函数名, 调用它的那一行), ...]，最外层的在前


# ======================================================================
# 1. 词法分析：把一长串字符切成一个个「记号」(token)
# ======================================================================

# 一个记号：kind 种类，value 值，line 行号，text 源代码里的原样写法。
# kind 的取值：NUM 数字、STR 文字、NAME 名字、KW 关键字、OP 运算符和标点、
#              NEWLINE 一行结束、INDENT 缩进增加、DEDENT 缩进减少、EOF 程序结束
Token = namedtuple('Token', 'kind value line text')

KEYWORD_LIST = [
    '函数', '返回', '全局',
    '如果', '否则', '否则如果',
    '当', '对于', '里的每个', '中的每个', '重复', '次', '跳出', '继续',
    '真', '假', '空',
]
KEYWORDS = set(KEYWORD_LIST)

# 用文字写的运算符：词法分析时直接换成对应的符号，后面的步骤就不用区分了。
# 这里的词都不能再当名字用，所以故意没有收「加」「减」「乘」：它们太适合当函数名了。
# 「除以」是例外，因为有些中文输入法会把 / 打成 、
WORD_OPERATORS = {
    '除以': '/',
    '大于': '>', '小于': '<', '大于等于': '>=', '小于等于': '<=',
    '等于': '==', '是': '==', '不等于': '!=',
    '并且': 'and', '或者': 'or', '不是': 'not',
}

# 几种看不见的空白。用编码来写，免得在编辑器里分不清谁是谁。
FULLWIDTH_SPACE = chr(0x3000)     # 全角空格
NO_BREAK_SPACE = chr(0xA0)        # 不换行空格，从网页复制代码时常会带进来
ZERO_WIDTH_SPACE = chr(0x200B)    # 零宽空格，同样常见于网页
BYTE_ORDER_MARK = chr(0xFEFF)     # 有些编辑器加在文件开头的标记

# 中文标点 -> 英文标点。
# 全角的 （ ） ， ： ； ！ ＝ 这些可以用公式换算（见下面的 halfwidth），
# 这张表只列公式换算不了的那些。原则是：同一个按键，中文输入法和英文输入法
# 打出来的符号，意思一样。
PUNCTUATION = {
    '【': '[', '】': ']', '、': ',', '。': ';', '《': '<', '》': '>',
    '×': '*', '÷': '/', '≤': '<=', '≥': '>=', '≠': '!=',
    FULLWIDTH_SPACE: ' ', NO_BREAK_SPACE: ' ', ZERO_WIDTH_SPACE: ' ', BYTE_ORDER_MARK: ' ',
}

# 开头的引号 -> 可以用来结尾的引号
QUOTES = {
    '"': '"', "'": "'",
    '“': '”“', '”': '”“',
    '‘': '’‘', '’': '’‘',
    '「': '」', '『': '』',
}

# 行首的空白各算几格缩进：全角空格算两格，Tab 算四格
INDENT_WIDTH = {
    ' ': 1, '\t': 4, FULLWIDTH_SPACE: 2,
    NO_BREAK_SPACE: 1, ZERO_WIDTH_SPACE: 0, BYTE_ORDER_MARK: 0,
}

BRACKETS = {'(': ')', '[': ']'}
TWO_CHAR_OPS = {'==', '!=', '<=', '>=', '+=', '-=', '*=', '/='}
ONE_CHAR_OPS = set('+-*/%=<>!()[],:;')
DIGITS = '0123456789'
ESCAPES = {'n': '\n', 't': '\t'}

# 两个字以上的关键字，长的排前面。报错时用来猜「是不是忘了加空格」。
RESERVED_LONG = sorted(
    [word for word in KEYWORD_LIST + list(WORD_OPERATORS) if len(word) >= 2],
    key=len, reverse=True)


# 名字里可以出现的文字：英文字母、带声调的拉丁字母、希腊和西里尔字母、假名、汉字、谚文。
# 用编码范围写死，这样解释器和用白话写的编译器对「什么算名字」的判断完全一致。
LETTER_RANGES = [
    (65, 90), (97, 122), (192, 214), (216, 246), (248, 591), (880, 1023), (1024, 1327),
    (12353, 12447), (12449, 12543), (13312, 19903), (19968, 40959), (44032, 55203),
    (63744, 64255), (131072, 201551),
]


def is_letter(ch):
    if len(ch) != 1:
        return False
    code = ord(ch)
    for low, high in LETTER_RANGES:
        if low <= code <= high:
            return True
    return False


def is_name_char(ch):
    return is_letter(ch) or ch == '_' or (len(ch) == 1 and ch in DIGITS)


def halfwidth(ch):
    """把中文（全角）符号换成对应的英文（半角）符号，其他字符原样返回。

    这个函数就是「支持中文标点」的全部秘密：词法分析每看一个字符，
    都先过一遍这里。引号里面的内容不经过这里，所以文字里的标点不会变。
    """
    if ch in PUNCTUATION:
        return PUNCTUATION[ch]
    if 0xFF01 <= ord(ch) <= 0xFF5E:       # 全角的 ！ 到 ～，编码和半角正好差 0xFEE0
        return chr(ord(ch) - 0xFEE0)
    return ch


def spacing_hint(names):
    """中文没有空格，很容易把关键字和名字连着写。名字里夹着关键字时给一句提示。"""
    for name in names:
        for word in RESERVED_LONG:
            if word in name and word != name:
                return ('\n提示：「%s」被当成了一个名字。如果其中的「%s」是单独的一个词，'
                        '要在它前后加上空格。' % (name, word))
    return ''


def tokenize(source):
    """词法分析：源代码字符串 -> 记号列表。"""
    tokens = []
    indents = [0]      # 缩进栈：每进入一层代码块，就把那一层的缩进宽度压进来
    brackets = []      # 还没关上的括号。括号里面可以随便换行

    def fail(message, line):
        raise BaihuaError(message, line, '语法错误')

    lines = source.replace('\r\n', '\n').replace('\r', '\n').split('\n')
    for lineno, text in enumerate(lines, 1):
        i, n = 0, len(text)

        if not brackets:
            # 一行的开头：量一量缩进
            width = 0
            while i < n and text[i] in INDENT_WIDTH:
                width += INDENT_WIDTH[text[i]]
                i += 1
            if i == n or halfwidth(text[i]) == '#':
                continue                      # 空行和整行注释直接跳过
            if width > indents[-1]:
                indents.append(width)
                tokens.append(Token('INDENT', None, lineno, ''))
            else:
                while width < indents[-1]:
                    indents.pop()
                    tokens.append(Token('DEDENT', None, lineno, ''))
                if width != indents[-1]:
                    fail('缩进对不齐：这一行开头的空格数和前面哪一层都对不上', lineno)

        while i < n:
            raw = text[i]            # 原始字符，可能是全角的
            ch = halfwidth(raw)      # 换算成半角之后的样子

            if ch == ' ' or ch == '\t':
                i += 1

            elif ch == '#':          # 注释：从 # 到行尾都不看
                break

            elif raw in QUOTES or ch in QUOTES:
                # 文字（字符串）。引号里面的内容原样保留
                closers = QUOTES.get(raw) or QUOTES[ch]
                chars = []
                j = i + 1
                while j < n and text[j] not in closers and halfwidth(text[j]) not in closers:
                    if text[j] == '\\' and j + 1 < n:     # 反斜杠转义：\n 换行，\t 制表符
                        j += 1
                        chars.append(ESCAPES.get(text[j], text[j]))
                    else:
                        chars.append(text[j])
                    j += 1
                if j >= n:
                    fail('文字的引号没有关上（开头的引号是 %s ）' % raw, lineno)
                tokens.append(Token('STR', ''.join(chars), lineno, text[i:j + 1]))
                i = j + 1

            elif ch in DIGITS:
                j = i
                while j < n and halfwidth(text[j]) in DIGITS:
                    j += 1
                whole = ''.join(halfwidth(c) for c in text[i:j])
                # 小数点：中文输入法可能把 . 打成 。 只要它后面紧跟着数字，就当小数点
                if j + 1 < n and text[j] in '.。．' and halfwidth(text[j + 1]) in DIGITS:
                    k = j + 1
                    while k < n and halfwidth(text[k]) in DIGITS:
                        k += 1
                    fraction = ''.join(halfwidth(c) for c in text[j + 1:k])
                    value, j = float(whole + '.' + fraction), k
                else:
                    value = int(whole)
                tokens.append(Token('NUM', value, lineno, text[i:j]))
                i = j

            elif is_letter(ch) or ch == '_':
                # 一串连续的汉字、字母、数字、下划线：可能是名字、关键字或文字运算符
                j = i
                while j < n:
                    c = halfwidth(text[j])
                    if not is_name_char(c):
                        break
                    j += 1
                word = ''.join(halfwidth(c) for c in text[i:j])
                if word in WORD_OPERATORS:
                    tokens.append(Token('OP', WORD_OPERATORS[word], lineno, word))
                elif word in KEYWORDS:
                    tokens.append(Token('KW', word, lineno, word))
                else:
                    tokens.append(Token('NAME', word, lineno, word))
                i = j

            else:
                following = halfwidth(text[i + 1]) if i + 1 < n else ''
                if len(ch) == 2:                      # ≤ ≥ ≠ 这种一个字符顶两个的
                    op, length = ch, 1
                elif ch + following in TWO_CHAR_OPS:  # == != <= >= += 等
                    op, length = ch + following, 2
                elif ch in ONE_CHAR_OPS:
                    op, length = ch, 1
                else:
                    fail('不认识的符号「%s」' % raw, lineno)
                if op in BRACKETS:
                    brackets.append((op, raw, lineno))
                elif op in (')', ']'):
                    if not brackets:
                        fail('多了一个「%s」，前面没有和它配对的括号' % raw, lineno)
                    opener, opener_raw, _ = brackets.pop()
                    if BRACKETS[opener] != op:
                        fail('括号没有配对：「%s」不能用「%s」来关' % (opener_raw, raw), lineno)
                tokens.append(Token('OP', op, lineno, text[i:i + length]))
                i += length

        if not brackets:
            tokens.append(Token('NEWLINE', None, lineno, ''))

    if brackets:
        _, opener_raw, opener_line = brackets[-1]
        fail('括号「%s」没有关上' % opener_raw, opener_line)
    for _ in indents[1:]:
        tokens.append(Token('DEDENT', None, len(lines), ''))
    tokens.append(Token('EOF', None, len(lines), ''))
    return tokens


# ======================================================================
# 2. 语法分析：把记号排成一棵「语法树」
# ======================================================================
#
# 语法（方括号是可选，星号是重复零次或多次）：
#
#   语句   := 名字 = 表达式                         赋值，还有 += -= *= /=
#           | 表达式                                通常是函数调用
#           | 如果 表达式：块 [否则如果 表达式：块]* [否则：块]
#           | 当 表达式：块
#           | 对于 表达式 里的每个 名字：块
#           | 重复 表达式 次：块
#           | 函数 名字（参数，参数）：块
#           | 返回 [表达式] | 跳出 | 继续 | 全局 名字
#   块     := 换行后缩进的若干条语句，或者直接写在冒号后面的一句
#
#   表达式的优先级，从低到高：
#       或者 < 并且 < 不是 < 比较（大于 小于 等于 是 ...）< + - < * / % < 负号 < 调用和下标
#   列表：【1，2，3】   字典：【“甲”：1，“乙”：2】   空字典：【：】

ASSIGN_OPS = {'=', '+=', '-=', '*=', '/='}
COMPARE_OPS = {'==', '!=', '<', '>', '<=', '>='}
CONSTANTS = {'真': True, '假': False, '空': None}
SYMBOL_NAMES = {':': '冒号「：」', '(': '左括号「（」', ')': '右括号「）」', ']': '右方括号「】」'}


def describe(tok):
    """把一个记号说成人话，报错时用。"""
    plain = {'NEWLINE': '换行', 'INDENT': '缩进', 'DEDENT': '缩进的结束', 'EOF': '程序的结尾'}
    return plain.get(tok.kind) or '「%s」' % tok.text


class Parser:
    """递归下降语法分析器：每个 parse_xxx 方法负责认出一种语法结构，返回语法树节点。

    节点就是普通的元组 (种类, 行号, 其他内容...)，例如：
        ('num', 3, 42)                          第 3 行的数字 42
        ('binop', 3, '+', 左边的节点, 右边的节点)   第 3 行的一次加法
    """

    def __init__(self, tokens):
        self.tokens = tokens
        self.pos = 0
        self.loop_depth = 0        # 现在套在几层循环里（检查「跳出」「继续」用）
        self.function_depth = 0    # 现在套在几层函数里（检查「返回」用）

    # ---------- 小工具 ----------

    def peek(self):
        return self.tokens[self.pos]

    def advance(self):
        tok = self.tokens[self.pos]
        if tok.kind != 'EOF':
            self.pos += 1
        return tok

    def check(self, kind, value=None):
        tok = self.peek()
        return tok.kind == kind and (value is None or tok.value == value)

    def accept(self, kind, value=None):
        return self.advance() if self.check(kind, value) else None

    def error(self, message, tok=None):
        tok = tok or self.peek()
        line = tok.line
        if tok.kind == 'EOF' and self.pos > 0:      # 程序突然结束：指向最后一行有内容的地方
            line = self.tokens[self.pos - 1].line
        names = [t.value for t in self.tokens if t.kind == 'NAME' and t.line == line]
        raise BaihuaError(message + spacing_hint(names), line, '语法错误')

    def expect(self, kind, value):
        if not self.check(kind, value):
            wanted = SYMBOL_NAMES.get(value) or '「%s」' % value
            self.error('这里需要%s，但出现的是%s' % (wanted, describe(self.peek())))
        return self.advance()

    def expect_name(self, what):
        tok = self.peek()
        if tok.kind == 'NAME':
            return self.advance().value
        if tok.kind == 'KW' or tok.text in WORD_OPERATORS:
            self.error('「%s」是白话语言自己要用的词，不能拿来当%s' % (tok.text, what))
        self.error('这里需要一个%s，但出现的是%s' % (what, describe(tok)))

    def at_statement_end(self):
        return self.check('NEWLINE') or self.check('EOF') or self.check('OP', ';')

    # ---------- 语句 ----------

    def parse_program(self):
        statements = []
        while not self.check('EOF'):
            statements.extend(self.parse_statement())
        return statements

    def parse_statement(self):
        """解析一行。返回列表，因为一行里可以用 ； 或 。 隔开写好几句。"""
        tok = self.peek()
        if tok.kind == 'INDENT':
            self.error('这一行不该缩进：只有冒号下面的内容才需要缩进')
        if tok.kind == 'KW':
            if tok.value == '如果':
                return [self.parse_if()]
            if tok.value == '当':
                return [self.parse_while()]
            if tok.value == '对于':
                return [self.parse_foreach()]
            if tok.value == '重复':
                return [self.parse_repeat()]
            if tok.value == '函数':
                return [self.parse_function()]
        return self.parse_simple_line()

    def parse_simple_line(self):
        statements = [self.parse_simple()]
        while self.accept('OP', ';'):
            if self.check('NEWLINE') or self.check('EOF'):
                break                               # 行尾的句号或分号，可写可不写
            statements.append(self.parse_simple())
        if not (self.check('EOF') or self.accept('NEWLINE')):
            tok = self.peek()
            message = '这一句到这里应该结束了，但后面还有%s' % describe(tok)
            last = statements[-1]
            if last[0] == 'expr' and last[2][0] == 'name' and tok.kind in ('NUM', 'STR', 'NAME'):
                name = last[2][2]
                message += '\n提示：如果是想调用「%s」，要加括号，例如：%s（%s）' % (name, name, tok.text)
            elif tok.text == '、':
                message += '\n提示：顿号在这里相当于逗号。如果想做除法，请写 / 或者「除以」。'
            self.error(message)
        return statements

    def parse_simple(self):
        tok = self.peek()
        line = tok.line
        if tok.kind == 'KW':
            if tok.value == '返回':
                if self.function_depth == 0:
                    self.error('「返回」只能写在函数里面')
                self.advance()
                value = None if self.at_statement_end() else self.parse_expression()
                return ('return', line, value)
            if tok.value in ('跳出', '继续'):
                if self.loop_depth == 0:
                    self.error('「%s」只能写在循环里面' % tok.value)
                self.advance()
                return ('break' if tok.value == '跳出' else 'continue', line)
            if tok.value == '全局':
                self.advance()
                names = [self.expect_name('变量名')]
                while self.accept('OP', ','):
                    names.append(self.expect_name('变量名'))
                return ('global', line, names)

        expression = self.parse_expression()
        op = self.peek()
        if op.kind == 'OP' and op.value in ASSIGN_OPS:
            if expression[0] not in ('name', 'index'):
                self.error('「%s」的左边只能是名字，或者 列表【下标】' % op.text)
            self.advance()
            return ('assign', line, expression, op.value, self.parse_expression())
        return ('expr', line, expression)

    def parse_block(self):
        """冒号后面的代码块：换行后缩进的若干行，或者直接写在同一行的一句。"""
        if self.check('OP', '='):
            self.error('判断是否相等要写「==」「等于」或者「是」，单独一个「=」是赋值')
        self.expect('OP', ':')
        if not self.check('NEWLINE'):
            return self.parse_simple_line()         # 如果 甲 大于 乙：打印（甲）
        self.advance()
        if not self.accept('INDENT'):
            self.error('冒号下面的内容需要缩进（开头空四格）')
        statements = []
        while not self.check('DEDENT') and not self.check('EOF'):
            statements.extend(self.parse_statement())
        self.accept('DEDENT')
        return statements

    def parse_loop_body(self):
        self.loop_depth += 1
        body = self.parse_block()
        self.loop_depth -= 1
        return body

    def parse_if(self):
        line = self.advance().line                  # 吃掉「如果」
        branches = [(self.parse_expression(), self.parse_block())]
        otherwise = None
        while True:
            if self.accept('KW', '否则如果'):
                branches.append((self.parse_expression(), self.parse_block()))
            elif self.accept('KW', '否则'):
                if self.accept('KW', '如果'):         # 「否则 如果」中间有空格也行
                    branches.append((self.parse_expression(), self.parse_block()))
                else:
                    otherwise = self.parse_block()
                    break
            else:
                break
        return ('if', line, branches, otherwise)

    def parse_while(self):
        line = self.advance().line                  # 吃掉「当」
        condition = self.parse_expression()
        return ('while', line, condition, self.parse_loop_body())

    def parse_foreach(self):
        line = self.advance().line                  # 吃掉「对于」
        iterable = self.parse_expression()
        if not (self.accept('KW', '里的每个') or self.accept('KW', '中的每个')):
            self.error('「对于」的写法是：对于 列表 里的每个 名字：')
        name = self.expect_name('变量名')
        return ('foreach', line, iterable, name, self.parse_loop_body())

    def parse_repeat(self):
        line = self.advance().line                  # 吃掉「重复」
        count = self.parse_expression()
        if not self.accept('KW', '次'):
            self.error('「重复」的写法是：重复 3 次：')
        return ('repeat', line, count, self.parse_loop_body())

    def parse_function(self):
        line = self.advance().line                  # 吃掉「函数」
        name = self.expect_name('函数名')
        self.expect('OP', '(')
        params = []
        while not self.check('OP', ')'):
            param = self.expect_name('参数名')
            if param in params:
                self.error('参数名「%s」重复了' % param)
            params.append(param)
            if not self.accept('OP', ','):
                break
        self.expect('OP', ')')
        # 函数体是一个全新的环境：外面的循环管不到里面的「跳出」
        outer_loops, self.loop_depth = self.loop_depth, 0
        self.function_depth += 1
        body = self.parse_block()
        self.function_depth -= 1
        self.loop_depth = outer_loops
        return ('def', line, name, params, body)

    # ---------- 表达式：优先级越低的越先被调用 ----------

    def parse_expression(self):
        return self.parse_or()

    def parse_or(self):
        left = self.parse_and()
        while self.check('OP', 'or'):
            line = self.advance().line
            left = ('or', line, left, self.parse_and())
        return left

    def parse_and(self):
        left = self.parse_not()
        while self.check('OP', 'and'):
            line = self.advance().line
            left = ('and', line, left, self.parse_not())
        return left

    def parse_not(self):
        if self.check('OP', 'not') or self.check('OP', '!'):
            line = self.advance().line              # 写在最前面的「不是」：取反
            return ('not', line, self.parse_not())
        return self.parse_comparison()

    def parse_comparison(self):
        first = self.parse_additive()
        line = self.peek().line
        rest = []
        while True:
            tok = self.peek()
            if tok.kind == 'OP' and tok.value in COMPARE_OPS:
                op = tok.value
            elif tok.kind == 'OP' and tok.value == 'not':
                op = '!='                           # 夹在两个值中间的「不是」：不等于
            else:
                break
            self.advance()
            rest.append((op, self.parse_additive()))
        # 可以连着比：1 小于 x 小于 10
        return ('compare', line, first, rest) if rest else first

    def parse_additive(self):
        left = self.parse_term()
        while self.peek().kind == 'OP' and self.peek().value in ('+', '-'):
            tok = self.advance()
            left = ('binop', tok.line, tok.value, left, self.parse_term())
        return left

    def parse_term(self):
        left = self.parse_unary()
        while self.peek().kind == 'OP' and self.peek().value in ('*', '/', '%'):
            tok = self.advance()
            left = ('binop', tok.line, tok.value, left, self.parse_unary())
        return left

    def parse_unary(self):
        if self.check('OP', '-'):
            line = self.advance().line
            return ('neg', line, self.parse_unary())
        return self.parse_postfix()

    def parse_postfix(self):
        node = self.parse_primary()
        while True:
            if self.check('OP', '('):               # 函数调用：名字（参数，参数）
                line = self.advance().line
                node = ('call', line, node, self.parse_items(')'))
            elif self.check('OP', '['):             # 取下标：列表【0】
                line = self.advance().line
                index = self.parse_expression()
                self.expect('OP', ']')
                node = ('index', line, node, index)
            else:
                return node

    def parse_items(self, closer):
        """解析用逗号隔开的一串表达式，直到遇见右括号 closer。"""
        items = []
        while not self.check('OP', closer):
            items.append(self.parse_expression())
            if not self.accept('OP', ','):
                break
        self.expect('OP', closer)
        return items

    def parse_primary(self):
        tok = self.peek()
        if tok.kind == 'NUM':
            return ('num', self.advance().line, tok.value)
        if tok.kind == 'STR':
            return ('str', self.advance().line, tok.value)
        if tok.kind == 'NAME':
            return ('name', self.advance().line, tok.value)
        if tok.kind == 'KW' and tok.value in CONSTANTS:
            return ('const', self.advance().line, CONSTANTS[tok.value])
        if tok.kind == 'OP' and tok.value == '(':
            self.advance()
            expression = self.parse_expression()
            self.expect('OP', ')')
            return expression
        if tok.kind == 'OP' and tok.value == '[':   # 列表：【1，2，3】  字典：【“甲”：1，“乙”：2】  空字典：【：】
            self.advance()
            if self.accept('OP', ':'):
                self.expect('OP', ']')
                return ('dict', tok.line, [])
            if self.accept('OP', ']'):
                return ('list', tok.line, [])
            first = self.parse_expression()
            if not self.accept('OP', ':'):
                items = [first]
                if self.accept('OP', ','):
                    items += self.parse_items(']')
                else:
                    self.expect('OP', ']')
                return ('list', tok.line, items)
            pairs = [(first, self.parse_expression())]
            while self.accept('OP', ','):
                if self.check('OP', ']'):
                    break
                key = self.parse_expression()
                self.expect('OP', ':')
                pairs.append((key, self.parse_expression()))
            self.expect('OP', ']')
            return ('dict', tok.line, pairs)
        if tok.kind == 'KW':
            self.error('「%s」不能出现在这里（它是白话语言的关键字，也不能当名字用）' % tok.text)
        self.error('这里缺了一个值，出现的是%s' % describe(tok))


# ======================================================================
# 3. 解释执行：沿着语法树一边走一边算
# ======================================================================

class Function:
    """用「函数」定义出来的函数。closure 是定义它时所在的环境。"""

    def __init__(self, name, params, body, closure):
        self.name = name
        self.params = params
        self.body = body
        self.closure = closure


# 内置函数：名字、对应的 Python 函数、最少和最多几个参数（max_args 为 None 表示不限）
Builtin = namedtuple('Builtin', 'name fn min_args max_args')


class Env:
    """环境（作用域）：记着一批变量。最外层有一个，每次调用函数再新建一个。"""

    def __init__(self, parent=None):
        self.vars = {}
        self.parent = parent
        self.global_names = set()    # 用「全局」声明过的名字
        self.outer_reads = set()     # 从外层读过的名字（用来发现「在函数里改外面的变量」）

    def find(self, name):
        """从里往外找，返回这个名字所在的环境。找不到返回 None。"""
        env = self
        while env is not None:
            if name in env.vars:
                return env
            env = env.parent
        return None


# 「返回」「跳出」「继续」要一下子跳出好几层，用异常来实现最省事
class ReturnSignal(Exception):
    def __init__(self, value):
        super().__init__()
        self.value = value


class BreakSignal(Exception):
    pass


class ContinueSignal(Exception):
    pass


def is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def type_name(value):
    if value is None:
        return '空'
    if isinstance(value, bool):
        return '真假'
    if isinstance(value, int):
        return '整数'
    if isinstance(value, float):
        return '小数'
    if isinstance(value, str):
        return '文字'
    if isinstance(value, list):
        return '列表'
    if isinstance(value, dict):
        return '字典'
    return '函数'


def show(value, quote=False):
    """把一个值变成给人看的文字。quote 为真时文字两边带引号（列表里面用）。"""
    if value is None:
        return '空'
    if value is True:
        return '真'
    if value is False:
        return '假'
    if isinstance(value, float):
        return '%.12g' % value                 # 0.1 + 0.2 显示成 0.3，5.0 显示成 5
    if isinstance(value, str):
        return '“%s”' % value if quote else value
    if isinstance(value, list):
        return '【' + '，'.join([show(item, True) for item in value]) + '】'
    if isinstance(value, dict):
        if not value:
            return '【：】'
        return '【' + '，'.join([show(key, True) + '：' + show(item, True) for key, item in value.items()]) + '】'
    if isinstance(value, Function):
        return '<函数 %s>' % value.name
    if isinstance(value, Builtin):
        return '<内置函数 %s>' % value.name
    return str(value)


def equal(a, b):
    if isinstance(a, bool) != isinstance(b, bool):   # 真 和 1 不算相等
        return False
    return a == b


# 字典直接用 Python 的 dict：条目保持放进去的先后顺序，1、1.0、真 算同一个键。
def check_key(key, line=None):
    if isinstance(key, float) and key != key:
        raise BaihuaError('nan 不能当字典的键', line)
    if not (key is None or isinstance(key, (bool, int, float, str))):
        raise BaihuaError('%s不能当字典的键（只有数字、文字、真假和空可以）' % type_name(key), line)


def dict_get(table, key, line=None):
    check_key(key, line)
    if key not in table:
        raise BaihuaError('字典里没有 %s 这个键' % show(key, True), line)
    return table[key]


OP_NAMES = {'+': '加', '-': '减', '*': '乘', '/': '除', '%': '取余数'}


class Interpreter:

    def __init__(self):
        self.globals = Env()
        for builtin in BUILTINS:
            self.globals.vars[builtin.name] = builtin
        self.echo = False     # 交互模式：把每句表达式的结果显示出来
        self.line = None      # 正在执行的那一行
        # 正在进行的函数调用：[(函数名, 调用它的那一行), ...]。
        # 出错时不弹出，所以报错的时候还能看到出错那一刻的调用过程
        self.frames = []

    def run(self, source):
        self.line = None
        self.frames = []
        try:
            tree = Parser(tokenize(source)).parse_program()
            self.execute_block(tree, self.globals)
        except BaihuaError as error:
            error.frames = list(self.frames)
            raise
        except RecursionError:
            message = '嵌套得太深了。如果是函数自己调用自己，检查一下是不是缺少出口'
            if sys.version_info < (3, 11):
                message += '（这个版本的 Python 只能递归不到 200 层，换成 3.11 以上可以到几千层）'
            error = BaihuaError(message, self.line)
            error.frames = list(self.frames)
            raise error from None
        except OverflowError:
            error = BaihuaError('数字太大了，算不了', self.line)
            error.frames = list(self.frames)
            raise error from None

    # ---------- 语句 ----------

    def execute_block(self, statements, env):
        for statement in statements:
            self.execute(statement, env)

    def execute(self, node, env):
        kind, line = node[0], node[1]
        self.line = line

        if kind == 'expr':
            value = self.evaluate(node[2], env)
            if self.echo and env is self.globals and value is not None:
                print(show(value, quote=True))

        elif kind == 'assign':
            target, op, value = node[2], node[3], self.evaluate(node[4], env)
            if op != '=':                           # 甲 += 1 就是 甲 = 甲 + 1
                value = self.binary(op[0], self.evaluate(target, env), value, line)
            if target[0] == 'name':
                self.set_var(target[2], value, env, line)
            else:                                   # 列表【下标】 = 值，字典【键】 = 值
                container = self.evaluate(target[2], env)
                index = self.evaluate(target[3], env)
                if isinstance(container, dict):
                    check_key(index, line)
                    container[index] = value
                    return
                if not isinstance(container, list):
                    raise BaihuaError('只有列表和字典里的东西可以这样修改，这里是%s' % type_name(container), line)
                self.check_index(container, index, line)
                container[index] = value

        elif kind == 'if':
            for condition, body in node[2]:
                if self.evaluate(condition, env):
                    self.execute_block(body, env)
                    return
            if node[3] is not None:
                self.execute_block(node[3], env)

        elif kind == 'while':
            while self.evaluate(node[2], env):
                if not self.run_loop_body(node[3], env):
                    break

        elif kind == 'foreach':
            items = self.evaluate(node[2], env)
            if not isinstance(items, (list, str, dict)):
                raise BaihuaError('「对于」只能逐个取出列表、文字或字典里的东西，这里是%s' % type_name(items), line)
            for item in list(items):                # 字典：逐个取出键
                self.set_var(node[3], item, env, line)
                if not self.run_loop_body(node[4], env):
                    break

        elif kind == 'repeat':
            count = self.evaluate(node[2], env)
            if not is_int(count):
                raise BaihuaError('「重复」和「次」中间要放一个整数，这里是%s' % type_name(count), line)
            for _ in range(count):
                if not self.run_loop_body(node[3], env):
                    break

        elif kind == 'def':
            self.set_var(node[2], Function(node[2], node[3], node[4], env), env, line)

        elif kind == 'return':
            raise ReturnSignal(None if node[2] is None else self.evaluate(node[2], env))

        elif kind == 'break':
            raise BreakSignal()

        elif kind == 'continue':
            raise ContinueSignal()

        elif kind == 'global':
            if env is not self.globals:
                env.global_names.update(node[2])

    def run_loop_body(self, body, env):
        """把循环体执行一遍。返回 False 表示遇到了「跳出」，循环该停了。"""
        try:
            self.execute_block(body, env)
        except ContinueSignal:
            pass
        except BreakSignal:
            return False
        return True

    # ---------- 变量 ----------
    # 规则和 Python 一样：读变量时从里往外找；在函数里赋值只影响函数自己的变量，
    # 想改最外层的变量，要先写「全局 名字」。

    def lookup(self, name, env, line):
        if name in env.global_names:
            found = self.globals if name in self.globals.vars else None
        else:
            found = env.find(name)
            if found is not None and found is not env:
                env.outer_reads.add(name)
        if found is None:
            raise BaihuaError('找不到名字「%s」，是不是还没有赋值，或者打错字了？' % name
                              + spacing_hint([name]), line)
        return found.vars[name]

    def set_var(self, name, value, env, line):
        if name in env.global_names:
            self.globals.vars[name] = value
        elif env is not self.globals and name not in env.vars and name in env.outer_reads:
            raise BaihuaError('函数里面不能直接修改外面的变量「%s」。'
                              '如果它是最外层的变量，先在函数里写一行：全局 %s' % (name, name), line)
        else:
            env.vars[name] = value

    # ---------- 表达式 ----------

    def evaluate(self, node, env):
        kind, line = node[0], node[1]

        if kind in ('num', 'str', 'const'):
            return node[2]

        if kind == 'name':
            return self.lookup(node[2], env, line)

        if kind == 'list':
            return [self.evaluate(item, env) for item in node[2]]

        if kind == 'dict':                          # 先从左到右算出所有的键和值，再一起放进字典
            pairs = [(self.evaluate(key, env), self.evaluate(item, env)) for key, item in node[2]]
            table = {}
            for key, item in pairs:
                check_key(key, line)
                table[key] = item
            return table

        if kind == 'binop':
            return self.binary(node[2], self.evaluate(node[3], env), self.evaluate(node[4], env), line)

        if kind == 'compare':
            left = self.evaluate(node[2], env)
            for op, right_node in node[3]:
                right = self.evaluate(right_node, env)
                if not self.compare(op, left, right, line):
                    return False
                left = right
            return True

        if kind == 'and':                           # 左边不成立，就不用看右边了
            left = self.evaluate(node[2], env)
            return self.evaluate(node[3], env) if left else left

        if kind == 'or':
            left = self.evaluate(node[2], env)
            return left if left else self.evaluate(node[3], env)

        if kind == 'not':
            return not self.evaluate(node[2], env)

        if kind == 'neg':
            value = self.evaluate(node[2], env)
            if not is_number(value):
                raise BaihuaError('负号只能用在数字前面，这里是%s' % type_name(value), line)
            return -value

        if kind == 'index':
            container = self.evaluate(node[2], env)
            index = self.evaluate(node[3], env)
            if isinstance(container, dict):
                return dict_get(container, index, line)
            if not isinstance(container, (list, str)):
                raise BaihuaError('只有列表、文字和字典可以用【下标】取值，这里是%s' % type_name(container), line)
            self.check_index(container, index, line)
            return container[index]

        if kind == 'call':
            function = self.evaluate(node[2], env)
            args = [self.evaluate(arg, env) for arg in node[3]]
            return self.call(function, args, line, node[2])

        raise AssertionError('解释器不认识的节点：%r' % (kind,))

    def check_index(self, container, index, line):
        if not is_int(index):
            raise BaihuaError('下标必须是整数，这里是%s' % type_name(index), line)
        if not -len(container) <= index < len(container):
            raise BaihuaError('下标 %d 超出了范围（一共 %d 个，下标从 0 开始数）'
                              % (index, len(container)), line)

    def binary(self, op, a, b, line):
        if op == '+':
            if isinstance(a, str) or isinstance(b, str):
                return show(a) + show(b)            # 文字 + 任何东西 = 拼在一起
            if isinstance(a, list) and isinstance(b, list):
                return a + b
        if op == '*':                               # 文字或列表 乘以 整数 = 重复几遍
            if isinstance(a, (str, list)) and is_int(b):
                return a * b
            if isinstance(b, (str, list)) and is_int(a):
                return b * a
        if not (is_number(a) and is_number(b)):
            raise BaihuaError('%s和%s之间不能%s' % (type_name(a), type_name(b), OP_NAMES[op]), line)
        if op == '+':
            return a + b
        if op == '-':
            return a - b
        if op == '*':
            return a * b
        if b == 0:
            raise BaihuaError('除数不能是 0', line)
        if op == '%':
            return a % b
        if isinstance(a, int) and isinstance(b, int) and a % b == 0:
            return a // b                           # 能整除就保持整数：10 / 2 是 5，不是 5.0
        return a / b

    def compare(self, op, a, b, line):
        if op == '==':
            return equal(a, b)
        if op == '!=':
            return not equal(a, b)
        both_numbers = is_number(a) and is_number(b)
        both_text = isinstance(a, str) and isinstance(b, str)
        if not (both_numbers or both_text):
            raise BaihuaError('%s和%s没法比大小' % (type_name(a), type_name(b)), line)
        if op == '<':
            return a < b
        if op == '>':
            return a > b
        if op == '<=':
            return a <= b
        return a >= b

    def call(self, function, args, line, callee):
        if isinstance(function, Builtin):
            low, high, count = function.min_args, function.max_args, len(args)
            if count < low or (high is not None and count > high):
                if high is None:
                    need = '至少需要 %d 个参数' % low
                elif low == high:
                    need = '需要 %d 个参数' % low
                else:
                    need = '需要 %d 到 %d 个参数' % (low, high)
                raise BaihuaError('「%s」%s，这里给了 %d 个' % (function.name, need, count), line)
            try:
                return function.fn(*args)
            except BaihuaError as error:
                if error.line is None:
                    error.line = line
                raise

        if isinstance(function, Function):
            if len(args) != len(function.params):
                raise BaihuaError('函数「%s」需要 %d 个参数，这里给了 %d 个'
                                  % (function.name, len(function.params), len(args)), line)
            local = Env(function.closure)           # 每次调用都有一个新环境
            local.vars.update(zip(function.params, args))
            self.frames.append((function.name, line))
            try:
                self.execute_block(function.body, local)
            except ReturnSignal as signal:
                self.frames.pop()
                return signal.value
            self.frames.pop()                       # 出错时不弹出（见 __init__）
            return None

        what = '「%s」' % callee[2] if callee[0] == 'name' else '这个值'
        raise BaihuaError('%s是%s，不是函数，不能加括号调用' % (what, type_name(function)), line)


# ======================================================================
# 4. 内置函数
# ======================================================================

def builtin_print(*args):
    print(' '.join([show(arg) for arg in args]))


def builtin_input(prompt=''):
    try:
        return input(show(prompt))
    except EOFError:
        raise BaihuaError('没有可以读取的输入了') from None


def builtin_length(value):
    if not isinstance(value, (str, list, dict)):
        raise BaihuaError('只有文字、列表和字典有长度，这里是%s' % type_name(value))
    return len(value)


def builtin_range(*args):
    """范围（1，5）是【1，2，3，4，5】，两头都算。范围（5）从 1 开始。第三个参数是步长。"""
    if not all([is_int(arg) for arg in args]):
        raise BaihuaError('「范围」的参数必须是整数')
    if len(args) == 1:
        start, stop, step = 1, args[0], 1
    else:
        start, stop, step = args[0], args[1], (args[2] if len(args) == 3 else 1)
    if step == 0:
        raise BaihuaError('「范围」的步长不能是 0')
    return list(range(start, stop + (1 if step > 0 else -1), step))


def builtin_int(value):
    try:
        if isinstance(value, str):
            try:
                return int(value.strip())
            except ValueError:
                return int(float(value.strip()))
        if is_number(value):
            return int(value)
    except (ValueError, OverflowError):
        pass
    raise BaihuaError('没法把 %s 变成整数' % show(value, True))


def builtin_float(value):
    try:
        if isinstance(value, str):
            return float(value.strip())
        if is_number(value):
            return float(value)
    except (ValueError, OverflowError):
        pass
    raise BaihuaError('没法把 %s 变成小数' % show(value, True))


def builtin_append(items, value):
    if not isinstance(items, list):
        raise BaihuaError('「添加」的第一个参数必须是列表，这里是%s' % type_name(items))
    items.append(value)


def builtin_remove(items, index):
    if isinstance(items, dict):                 # 字典：删掉这个键，返回它的值
        value = dict_get(items, index)
        del items[index]
        return value
    if not isinstance(items, list):
        raise BaihuaError('「删除」的第一个参数必须是列表或字典，这里是%s' % type_name(items))
    if not is_int(index) or not -len(items) <= index < len(items):
        raise BaihuaError('「删除」的第二个参数是下标，%s 不在范围里（一共 %d 个，下标从 0 开始数）'
                          % (show(index, True), len(items)))
    return items.pop(index)


def builtin_sorted(items):
    if not isinstance(items, list):
        raise BaihuaError('「排序」的参数必须是列表，这里是%s' % type_name(items))
    try:
        return sorted(items)
    except TypeError:
        raise BaihuaError('列表里的东西不是同一类，没法排序') from None


def builtin_randint(low, high):
    if not (is_int(low) and is_int(high)) or low > high:
        raise BaihuaError('「随机整数」需要两个整数，而且前一个不能比后一个大')
    return random.randint(low, high)


def builtin_slice(value, start, length):
    if not isinstance(value, (str, list)):
        raise BaihuaError('「截取」的第一个参数必须是文字或列表，这里是%s' % type_name(value))
    if not (is_int(start) and is_int(length)) or start < 0 or length < 0:
        raise BaihuaError('「截取」的开始位置和长度必须是不小于 0 的整数')
    return value[start:start + length]


def builtin_ord(ch):
    if not isinstance(ch, str) or len(ch) != 1:
        raise BaihuaError('「编码」需要一个单独的字，这里是 %s' % show(ch, True))
    return ord(ch)


def builtin_chr(code):
    if not is_int(code) or not 0 <= code <= 0x10FFFF or 0xD800 <= code <= 0xDFFF:
        raise BaihuaError('「字符」需要一个有效的文字编码，这里是 %s' % show(code, True))
    return chr(code)


def builtin_join(items, separator=''):
    if not isinstance(items, list):
        raise BaihuaError('「连接」的第一个参数必须是列表，这里是%s' % type_name(items))
    if not isinstance(separator, str):
        raise BaihuaError('「连接」的分隔符必须是文字，这里是%s' % type_name(separator))
    return separator.join([show(item) for item in items])


def builtin_find(container, value):
    if isinstance(container, str):
        if not isinstance(value, str):
            raise BaihuaError('在文字里只能查找文字，这里是%s' % type_name(value))
        return container.find(value)
    if isinstance(container, list):
        for position, item in enumerate(container):
            if equal(item, value):
                return position
        return -1
    raise BaihuaError('「查找」的第一个参数必须是文字或列表，这里是%s' % type_name(container))


def builtin_split(text, separator):
    if not isinstance(text, str) or not isinstance(separator, str):
        raise BaihuaError('「分割」的两个参数都必须是文字')
    if separator == '':
        raise BaihuaError('「分割」的分隔符不能是空的')
    return text.split(separator)


def builtin_read_file(path):
    if not isinstance(path, str):
        raise BaihuaError('文件路径必须是文字，这里是%s' % type_name(path))
    try:
        with open(path, 'rb') as f:
            return f.read().decode('utf-8', 'replace')
    except OSError:
        return None            # 打不开就返回 空，由程序自己决定怎么办


def builtin_write_file(path, content):
    if not isinstance(path, str):
        raise BaihuaError('文件路径必须是文字，这里是%s' % type_name(path))
    if not isinstance(content, str):
        raise BaihuaError('「写文件」的内容必须是文字，这里是%s' % type_name(content))
    try:
        with open(path, 'wb') as f:
            f.write(content.encode('utf-8'))
        return True
    except OSError:
        return False           # 写成功返回 真，失败返回 假


PROGRAM_ARGS = []      # 运行 python3 baihua.py 程序.bh 甲 乙 时，这里是 ['甲', '乙']


def builtin_args():
    return list(PROGRAM_ARGS)


def builtin_exit(code=0):
    if not is_int(code):
        raise BaihuaError('「退出」的参数必须是整数，这里是%s' % type_name(code))
    sys.stdout.flush()
    raise SystemExit(code)


def builtin_system(command):
    if not isinstance(command, str):
        raise BaihuaError('「运行命令」的参数必须是文字，这里是%s' % type_name(command))
    sys.stdout.flush()
    sys.stderr.flush()
    return subprocess.call(command, shell=True)


def builtin_eprint(*args):
    sys.stdout.flush()
    print(' '.join([show(arg) for arg in args]), file=sys.stderr)


# ---------- 0.3 版新增：字典、按行读文件、格式化、文字处理 ----------

def builtin_contains(container, item):
    if isinstance(container, dict):
        check_key(item)
        return item in container
    if isinstance(container, list):
        return any(equal(x, item) for x in container)
    if isinstance(container, str):
        if not isinstance(item, str):
            raise BaihuaError('在文字里只能查找文字，这里是%s' % type_name(item))
        return item in container
    raise BaihuaError('「包含」的第一个参数必须是列表、文字或字典，这里是%s' % type_name(container))


def need_dict(value, who):
    if not isinstance(value, dict):
        raise BaihuaError('「%s」的参数必须是字典，这里是%s' % (who, type_name(value)))
    return value


def builtin_keys(table):
    return list(need_dict(table, '所有键'))


def builtin_values(table):
    return list(need_dict(table, '所有值').values())


def builtin_items(table):
    return [[key, value] for key, value in need_dict(table, '所有项').items()]


def builtin_get(table, key, default=None):
    """取（字典，键，默认值）：有这个键就返回它的值，没有就返回默认值（不写默认值就是 空）。"""
    if not isinstance(table, dict):
        raise BaihuaError('「取」的第一个参数必须是字典，这里是%s' % type_name(table))
    check_key(key)
    return table[key] if key in table else default


def builtin_format(template, *values):
    """格式化（“{}今年{}岁”，名字，年龄）：模板里的 {} 依次换成后面的值。
    全角的 ｛｝ 也行；想要大括号本身就连写两个：{{ }}"""
    if not isinstance(template, str):
        raise BaihuaError('「格式化」的第一个参数必须是文字，这里是%s' % type_name(template))
    out, count, i, n = [], 0, 0, len(template)
    while i < n:
        c = template[i]
        if c in '{｛':
            close = '}' if c == '{' else '｝'
            if i + 1 < n and template[i + 1] == c:
                out.append(c)
                i += 2
                continue
            if i + 1 < n and template[i + 1] == close:
                if count < len(values):
                    out.append(show(values[count]))
                count += 1
                i += 2
                continue
        elif c in '}｝' and i + 1 < n and template[i + 1] == c:
            out.append(c)
            i += 2
            continue
        out.append(c)
        i += 1
    if count != len(values):
        raise BaihuaError('「格式化」的模板里有 %d 个 {}，后面却给了 %d 个值' % (count, len(values)))
    return ''.join(out)


def builtin_fixed(number, digits):
    """保留小数（3.14159，2）是 “3.14”：四舍五入到几位小数，结果是文字。"""
    if not is_number(number):
        raise BaihuaError('「保留小数」的第一个参数必须是数字，这里是%s' % type_name(number))
    if not is_int(digits) or not 0 <= digits <= 100:
        raise BaihuaError('「保留小数」的位数必须是 0 到 100 之间的整数')
    try:
        value = float(number)
    except OverflowError:
        raise BaihuaError('数字太大了，算不了') from None
    return '%.*f' % (digits, value)


def builtin_read_lines(path):
    """读行：读出整个文件，按行分开。每行末尾的换行不要；文件最后的换行不会多出一个空行。"""
    text = builtin_read_file(path)
    if text is None:
        return None
    lines = text.split('\n')
    if lines[-1] == '':
        lines.pop()
    return [line[:-1] if line.endswith('\r') else line for line in lines]


def builtin_append_file(path, content):
    if not isinstance(path, str):
        raise BaihuaError('文件路径必须是文字，这里是%s' % type_name(path))
    if not isinstance(content, str):
        raise BaihuaError('「追加文件」的内容必须是文字，这里是%s' % type_name(content))
    try:
        with open(path, 'ab') as f:
            f.write(content.encode('utf-8'))
        return True
    except OSError:
        return False


def builtin_replace(text, old, new):
    if not (isinstance(text, str) and isinstance(old, str) and isinstance(new, str)):
        raise BaihuaError('「替换」的三个参数都必须是文字')
    if old == '':
        raise BaihuaError('「替换」要换掉的文字不能是空的')
    return text.replace(old, new)


def builtin_strip(text):
    if not isinstance(text, str):
        raise BaihuaError('「去空白」的参数必须是文字，这里是%s' % type_name(text))
    return text.strip()


def builtin_runtime_source():
    """编译器专用：返回 C 运行时的源码（解释器里就是读旁边的 baihua_runtime.c）。"""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'baihua_runtime.c')
    try:
        with open(path, 'rb') as f:
            return f.read().decode('utf-8', 'replace')
    except OSError:
        raise BaihuaError('找不到运行时源码 baihua_runtime.c（它要和 baihua.py 放在一起）') from None


BUILTINS = [
    Builtin('打印', builtin_print, 0, None),
    Builtin('输入', builtin_input, 0, 1),
    Builtin('长度', builtin_length, 1, 1),
    Builtin('范围', builtin_range, 1, 3),
    Builtin('整数', builtin_int, 1, 1),
    Builtin('小数', builtin_float, 1, 1),
    Builtin('文字', show, 1, 1),
    Builtin('类型', type_name, 1, 1),
    Builtin('添加', builtin_append, 2, 2),
    Builtin('删除', builtin_remove, 2, 2),
    Builtin('排序', builtin_sorted, 1, 1),
    Builtin('随机整数', builtin_randint, 2, 2),
    Builtin('截取', builtin_slice, 3, 3),
    Builtin('编码', builtin_ord, 1, 1),
    Builtin('字符', builtin_chr, 1, 1),
    Builtin('连接', builtin_join, 1, 2),
    Builtin('查找', builtin_find, 2, 2),
    Builtin('分割', builtin_split, 2, 2),
    Builtin('读文件', builtin_read_file, 1, 1),
    Builtin('写文件', builtin_write_file, 2, 2),
    Builtin('命令行参数', builtin_args, 0, 0),
    Builtin('退出', builtin_exit, 0, 1),
    Builtin('运行命令', builtin_system, 1, 1),
    Builtin('报错', builtin_eprint, 0, None),
    Builtin('运行时源码', builtin_runtime_source, 0, 0),
    Builtin('包含', builtin_contains, 2, 2),
    Builtin('所有键', builtin_keys, 1, 1),
    Builtin('所有值', builtin_values, 1, 1),
    Builtin('所有项', builtin_items, 1, 1),
    Builtin('取', builtin_get, 2, 3),
    Builtin('格式化', builtin_format, 1, None),
    Builtin('保留小数', builtin_fixed, 2, 2),
    Builtin('读行', builtin_read_lines, 1, 1),
    Builtin('追加文件', builtin_append_file, 2, 2),
    Builtin('替换', builtin_replace, 3, 3),
    Builtin('去空白', builtin_strip, 1, 1),
]


# ======================================================================
# 5. 入口：运行文件，或者进入交互模式
# ======================================================================

def report(error, source):
    """把错误用中文报出来，并指出是哪一行。出错的地方在函数里的话，再列出调用过程。"""
    where = '（第 %d 行）' % error.line if error.line else ''
    print('%s%s：%s' % (error.kind, where, error.message), file=sys.stderr)
    lines = source.replace('\r\n', '\n').replace('\r', '\n').split('\n')
    if error.line and 1 <= error.line <= len(lines) and lines[error.line - 1].strip():
        print('  >  ' + lines[error.line - 1].strip(), file=sys.stderr)
    if error.frames:
        print_stack(error, lines)


def print_stack(error, lines):
    """从最外层往里，一层一层列出来。连着重复的几层（递归）并在一起；太多的话只列开头和结尾。
    （编译出来的程序在 baihua_runtime.c 的 bh_print_stack 里做同样的事，两边的输出一字不差）"""
    # 第 k 层：在哪个函数里（None 是最外层）、执行到第几行
    frames = list(zip([None] + [name for name, _ in error.frames],
                      [line for _, line in error.frames] + [error.line or 0]))
    # 分段：每段是 [开头, 几层一组, 重复几遍]。一组最多 3 层（甲调乙、乙又调甲这种也能认出来）
    units, i = [], 0
    while i < len(frames):
        size, times = 1, 1
        for p in (1, 2, 3):
            reps = 1
            while i + (reps + 1) * p <= len(frames) and frames[i + reps * p:i + (reps + 1) * p] == frames[i:i + p]:
                reps += 1
            if reps >= 2:
                size, times = p, reps
                break
        units.append((i, size, times))
        i += size * times

    def show_unit(unit):
        first, size, times = unit
        for name, line in frames[first:first + size]:
            text = '  函数「%s」，第 %d 行' % (name, line) if name is not None else '  最外层，第 %d 行' % line
            if 1 <= line <= len(lines) and lines[line - 1].strip():
                text += '：' + lines[line - 1].strip()
            print(text, file=sys.stderr)
        if times > 1 and size == 1:
            print('  （上面这一行又重复了 %d 次）' % (times - 1), file=sys.stderr)
        elif times > 1:
            print('  （上面这 %d 行又重复了 %d 次）' % (size, times - 1), file=sys.stderr)

    print('调用过程（从最外层到出错的地方）：', file=sys.stderr)
    if len(units) <= 24:
        for unit in units:
            show_unit(unit)
    else:
        for unit in units[:8]:
            show_unit(unit)
        omitted = sum(size * times for _, size, times in units[8:-16])
        print('  ……（中间省略了 %d 层）……' % omitted, file=sys.stderr)
        for unit in units[-16:]:
            show_unit(unit)


def run_source(interpreter, source):
    """运行一段代码。成功返回 True，出错就报错并返回 False。"""
    try:
        interpreter.run(source)
        return True
    except BaihuaError as error:
        sys.stdout.flush()
        report(error, source)
        return False


def print_tokens(source):
    for tok in tokenize(source):
        if tok.kind in ('NEWLINE', 'INDENT', 'DEDENT', 'EOF'):
            print('第 %2d 行  %s' % (tok.line, tok.kind))
        elif str(tok.value) != tok.text:
            print('第 %2d 行  %-5s %s  ->  %s' % (tok.line, tok.kind, tok.text, tok.value))
        else:
            print('第 %2d 行  %-5s %s' % (tok.line, tok.kind, tok.text))


def repl():
    try:
        import readline  # noqa: F401  有它的话，交互模式里可以用方向键改上一行
    except ImportError:
        pass
    interpreter = Interpreter()
    interpreter.echo = True
    print('白话 %s 交互模式。直接输入代码就行，输入「退出」离开。' % VERSION)
    while True:
        try:
            chunk = input('白话> ')
            if chunk.strip() in ('退出', '退出（）', '退出()'):
                break
            # 以冒号结尾，说明下面还有缩进的内容：一直读到空行为止
            if chunk.rstrip().endswith((':', '：')):
                while True:
                    more = input('  ... ')
                    if not more.strip():
                        break
                    chunk += '\n' + more
        except EOFError:
            print()
            break
        except KeyboardInterrupt:
            print()
            continue
        try:
            run_source(interpreter, chunk)
        except KeyboardInterrupt:
            print('\n已中断', file=sys.stderr)


def setup_streams():
    """Windows 上把输入输出重定向到文件时，默认编码不是 UTF-8，这里统一成 UTF-8。"""
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        try:
            if not stream.isatty():
                stream.reconfigure(encoding='utf-8')
        except (AttributeError, ValueError):
            pass


def main(argv):
    setup_streams()
    # 白话程序每递归一层，解释器自己要递归 5 层，所以把 Python 的递归上限调高。
    # 只在 3.11 以上这么做：更老的版本递归太深时 Python 会直接崩溃，保持默认值才安全。
    if sys.version_info >= (3, 11):
        sys.setrecursionlimit(20000)

    args = argv[1:]
    if args and args[0] in ('-h', '--help'):
        print(__doc__.strip())
        return 0
    show_tokens = bool(args) and args[0] == '--tokens'
    if show_tokens:
        args = args[1:]
        if not args:
            print('用法：python3 baihua.py --tokens 文件名', file=sys.stderr)
            return 2
    if not args:
        repl()
        return 0

    try:
        with open(args[0], encoding='utf-8-sig') as f:
            source = f.read()
    except OSError:
        print('打不开文件：%s' % args[0], file=sys.stderr)
        return 1
    except UnicodeDecodeError:
        print('文件 %s 不是 UTF-8 编码，请另存为 UTF-8 再试' % args[0], file=sys.stderr)
        return 1

    try:
        if show_tokens:
            try:
                print_tokens(source)
            except BaihuaError as error:
                report(error, source)
                return 1
            return 0
        PROGRAM_ARGS[:] = args[1:]
        ok = run_source(Interpreter(), source)
        sys.stdout.flush()
        return 0 if ok else 1
    except KeyboardInterrupt:
        print('\n已中断', file=sys.stderr)
        return 130
    except BrokenPipeError:
        # 输出接到了 head 之类的命令上，对方提前关掉了：安静地退出
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
