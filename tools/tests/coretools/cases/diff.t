# diff (the generated pairs r*a r*b are compared by counts: see the comment there)
%setup
printf 'one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n' > f1
printf 'one\ntwo\nTHREE\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\n' > f2
printf 'x\ny\n' > f3
printf 'same\nlines\n' > same
printf 'same\nlines\n' > same2
: > empty
printf 'a\nb' > nonl1
printf 'a\nc' > nonl2
printf 'a\nb\n' > withnl
printf 'a\nb' > nonlx
printf 'Hello\nWorld\n' > case1
printf 'hello\nWORLD\n' > case2
printf 'a  b\nc\td\n' > sp1
printf 'a b\nc d \n' > sp2
printf 'a  b\nc\td  \n' > sp3
printf 'a\n\nb\n' > bl1
printf 'a\n\nb\n\n' > bl2
printf 'a\r\nb\r\n' > crlf1
printf 'a\nb\n' > crlf2
printf 'a\000b\n' > bin1
printf 'a\000c\n' > bin2
printf 'a\nb\nc\na\nb\nc\n' > ab1
printf 'a\nb\nc\n' > ab2
mkdir -p d1/sub d2/sub d1/onlydir
printf 'x\n' > d1/common; printf 'y\n' > d2/common
printf 'same\n' > d1/sub/s; printf 'same\n' > d2/sub/s
printf 'old\n' > d1/sub/t; printf 'new\n' > d2/sub/t
printf 'o\n' > d1/only1; printf 'p\n' > d2/only2
printf 'q\n' > d1/onlydir/inner
mkdir d2/f1x
printf 'a\n\n' > r0a
printf 'da' > r0b
printf 'ca\ncb\n\n\n\nab\na\nac\nc\naa\n' > r1a
printf 'ca\nac\ncb\n\na\nc\n\n\nab\na\nac\naa\n' > r1b
printf '\naa\nbc\nb\nbb\naa\nba\na\nb\n\nac\nbb\n' > r2a
printf 'cb\n\naa\nbc\nb\nbb\naa\nba\nba\na\nb\nb\n\nb\nbb\n' > r2b
printf '' > r3a
printf '' > r3b
printf 'b\n\ng\nec\ne\ngf\ngd\n\n\n\n\n\nda\nc\n' > r4a
printf 'b\n\nec\ne\ngf\nfc\ngd\n\n\n\n\n\nda\nc\n' > r4b
printf ' x\n \nyy\ny\n\n \nx\n\n\n\n' > r5a
printf ' x\n\n \nyy\ny\n\nx\n\n\n\n\n' > r5b
printf '\nba\nbb\nbb\n\n\nb\nb\na\n\n\nbb\na\n' > r6a
printf '\nba\nbb\nbb\n\nba\nb\nb\n\n\nbb\n\na\n' > r6b
printf 'x \n\n\n \n\n' > r7a
printf 'x \n\nxx\n\n \n\n' > r7b
printf 'bb\nbb\n\n\n\n\na\na\nc\nab\nbc\n' > r8a
printf 'bb\nbb\n\n\n\n\na\na\nc\nab\nbc' > r8b
printf 'dh\n\nf\n\ngh\nb\ncc\n\n\n\nhc\nhf' > r9a
printf 'dh\n\n\nf\n\ngh\nb\ncc\n\n\n\nhc\nhf\n' > r9b
printf 'ab\n\n\n\na\na\nbb\nba\n' > r10a
printf 'ab\n\n\n\na\na\nbb\nba\n' > r10b
printf 'gc\nca\nc\nac\n\n\nb\naf\nhb\nad\n' > r11a
printf 'gc\nc\nac\n\n\nb\naf\nhb\nad\n' > r11b
printf 'a\nbb\nba\nba\na\na\nb\n' > r12a
printf 'a\nba\nba\nba\na\na\nb\n' > r12b
printf 'b\nb' > r13a
printf '' > r13b
printf '\nf\neb\n\n\n\n\ne\n\n\nc\n' > r14a
printf 'f\neg\n\n\n\n\ne\nhf\n\n\nc\n' > r14b
printf '\nab\n\naa' > r15a
printf '\nb\na\naa\n' > r15b
printf 'a\n\n' > r16a
printf 'a\nba\n' > r16b
printf '\na\n\n\nbd\n' > r17a
printf '\na\n\n\nbd\n' > r17b
printf 'd\n\nd\ncg\na\n\n\n\n' > r18a
printf 'd\n\na\ned\n\n' > r18b
printf 'b\n\nb\nb\n\nd\n\nc' > r19a
printf 'b\naa\nb\n\nb\nd\n\nc\n' > r19b
printf 'cb\nc\na\nc\nca\n\ncc\nc\nca\ncc\n' > r20a
printf 'cb\nc\na\nc\nca\n\ncc\nc\nca\ncc\n' > r20b
printf '' > r21a
printf '\n' > r21b
printf 'a\naa\nb\n\na\naa\n' > r22a
printf 'a\na\n\n\naa\na\n' > r22b
printf '\nb\nba\n\na\nb\naa\nbb\nbb\n' > r23a
printf '\nab\nb\nba\na\na\nb\naa\nb\nbb\nbb\n' > r23b
printf 'aa\n' > r24a
printf 'b\nba\n' > r24b
printf 'b\na\na\nb\na\n' > r25a
printf 'b\n\nb\n' > r25b
printf '\n\nd\ncb\nd\n\ndb\naa\ndd\nbc\n' > r26a
printf '\n\nd\ncb\nc\nd\n\ndb\nbc\n' > r26b
printf 'bc\nd\n\n\nba\n' > r27a
printf 'bc\n\nd\n\nba\n\n' > r27b
printf 'ba\ndd\nb\nc\na\nc\ncb\nba\nb\nd\ndd\na\n' > r28a
printf 'ba\ndd\nb\nc\na\nc\ncb\nba\nb\nd\ndd\n' > r28b
printf '\n\n \ny\n\n\n' > r29a
printf '\n\n \ny\n\n\n' > r29b
printf 'aa\n' > r30a
printf '' > r30b
printf 'c\nbc\n\n\n\nc\nab\na\naa\nbb' > r31a
printf 'c\nbc\n\n\nc\nab\na\nab\nbb\n' > r31b
printf '' > r32a
printf '' > r32b
printf 'd\n' > r33a
printf 'd\nbd\n\n' > r33b
printf 'a\nb\n' > r34a
printf 'a\n\nb\n' > r34b
printf 'fb\n\n\nd\n\nha\ng' > r35a
printf 'c\n\nd\n\nha\ng\n' > r35b
printf '\nb\nb\n\n\nba\nb\n\n' > r36a
printf '\nb\nb\nba\n\n\nba\nb\n\n\n' > r36b
printf '\nab\nb\na' > r37a
printf 'ab\nbb\na\n' > r37b
printf 'g\ng\nc\nc\n\nec\ndf\nh\nb\ndg\n\n' > r38a
printf 'g\ng\nc\nc\n\nec\ndf\nh\nb\ndg\n\n\n' > r38b
printf '\n\nc\n\n\n\n' > r39a
printf '\n\nc\n\na\n\n\n' > r39b
printf 'b\nb\nb\nba\na\n\n\n\n\na\na\nb\n' > r40a
printf 'b\nb\nb\nab\nba\na\n\n\n\n\na\na\nb' > r40b
printf '\nf\n\nd\n\n\n\ndb\nc\ne\nab\nfd\n\nf' > r41a
printf '\nf\n\nd\n\n\ndb\nc\ne\nab\nfd\n\nf\n' > r41b
printf '\nb\nba\nba\n\n\nc\na\na\nc\nac\nac\n\n' > r42a
printf 'b\nba\nba\n\n\nc\nc\na\na\nac\nac\n\n' > r42b
printf 'a\nb\n\n\na\na\n\nb\na\n\n\n\nab\n' > r43a
printf 'a\nb\n\n\na\na\n\nb\na\n\n\n\nab\n' > r43b
printf '\ny\n' > r44a
printf '\n\ny\n' > r44b
printf '\n\nb\n\n' > r45a
printf '\nbc\na\na\n' > r45b
printf '\n' > r46a
printf '\n' > r46b
printf 'a\nb\nbb\na\nb\nbb\nba\n\n\nbb\n' > r47a
printf 'a\nb\nbb\na\nb\nbb\nba\n\n\na\nbb\n' > r47b
printf 'b\na\n' > r48a
printf 'a\n' > r48b
printf 'ba\n\n\naa\n\nb\n\naa\nb\n\nb\na\n' > r49a
printf 'ba\n\n\naa\n\nb\n\n\naa\nab\nb\n\nb\na\n' > r49b
printf 'ef\nc\n' > r50a
printf 'c\nb\n' > r50b
printf 'e\nf\ncf\nb\nd\n\nae\nee\nfa\nad\n\n' > r51a
printf 'e\nf\ncf\nb\nd\n\nf\n\nee\nfa\nad\n\n' > r51b
printf '' > r52a
printf '' > r52b
printf 'xy\ny \n\n\n \n' > r53a
printf 'x \ny \n\n\n \n' > r53b
printf '' > r54a
printf '' > r54b
printf 'dd\n\n\n\n\n\nad\n\n' > r55a
printf 'dd\n\n\n\n\nad\n\n\n' > r55b
printf 'a\ncc\n' > r56a
printf 'a\ncc\nc\nc' > r56b
printf '\n\n' > r57a
printf '' > r57b
printf ' \nyy\nxx\nxx\nx\nxx\nyx\ny\nxy\n  \n' > r58a
printf ' \nyy\nxx\nxx\n\nx\nxx\nyx\ny\n\nxx\nxy\n  \n\n' > r58b
printf '' > r59a
printf '' > r59b
printf 'aa\n\n' > r60a
printf 'aa\n\n\n' > r60b
printf 'ba\nab\n\n\n\n\n\n\n\nab\na\n\n\n' > r61a
printf 'ba\nab\n\n\n\n\nab\na\n\nbb\n' > r61b
printf 'ab\na\naa\nba\na\nab' > r62a
printf 'ab\na\naa\nba\na\nab\n' > r62b
printf '\nf\nec\nd\ndh\n\n\nbh\nbf\nb\ng\n' > r63a
printf '\nf\nec\ncg\n\n\nbf\nb\n' > r63b
printf 'c\ncb\n\nb\nbc\n\n\nb\nca\nab\nc\nac\n\n' > r64a
printf 'c\ncb\ncb\nb\nbc\n\n\nb\nca\nab\nc\nac\n\n' > r64b
printf '\n\nab\nb\n\nc\nc\n\n\nac\n\n' > r65a
printf 'd\n\n\nab\n\nc\nc\ncd\n\n\nac\n\n' > r65b
printf 'c\ncc\nba\ncc\ncc\n\nac\n\nb\nb\nca\na\nc\nca\n' > r66a
printf 'b\ncc\nba\ncc\ncc\n\ncc\nac\n\nb\nb\nca\na\nc\nca\n' > r66b
printf '\nd\n\n\nb\n\nbc\n\ndb\nda\ncc\nd\n' > r67a
printf '\nd\n\n\nb\n\n\n\ndb\nda\ncc\nd\n' > r67b
printf 'xy\ny\nx\n\n\n' > r68a
printf '\nxy\ny\n\n\n' > r68b
printf 'b\n\n\n\n\nab\ncc\n\n\nc\nbb\nc\n' > r69a
printf '\n\nc\nb\n\n\nab\ncc\n\nc\nbb\nc\n' > r69b
%cases
diff same same2; echo "st=$?"
diff f1 f2; echo "st=$?"
diff -u f1 f2 | tail -n +3; echo "st=$?"
diff -u --label old --label new f1 f2
diff -c --label old --label new f1 f2
diff -U 1 --label old --label new f1 f2
diff -U0 --label old --label new f1 f2
diff -C 1 --label old --label new f1 f2
diff -e f1 f2
diff --normal f1 f2
diff -q f1 f2; echo "st=$?"
diff -q same same2; echo "st=$?"
diff -s same same2; echo "st=$?"
diff -s f1 f2 > /dev/null; echo "st=$?"
diff --brief f1 f2
diff empty f1
diff f1 empty
diff -u --label a --label b empty f1
diff -u --label a --label b f1 empty
diff -c --label a --label b empty f1
diff empty empty; echo "st=$?"
diff nonl1 nonl2
diff -u --label a --label b nonl1 nonl2
diff -u --label a --label b nonl1 f1
diff nonl1 nonl1; echo "st=$?"
diff withnl nonlx
diff -u --label a --label b withnl nonlx
diff -c --label a --label b withnl nonlx
diff case1 case2; echo "st=$?"
diff -i case1 case2; echo "st=$?"
diff sp1 sp2; echo "st=$?"
diff -b sp1 sp2; echo "st=$?"
diff -w sp1 sp2; echo "st=$?"
diff -Z sp1 sp3; echo "st=$?"
diff -B bl1 bl2; echo "st=$?"
diff bl1 bl2; echo "st=$?"
diff crlf1 crlf2; echo "st=$?"
diff --strip-trailing-cr crlf1 crlf2; echo "st=$?"
diff bin1 bin2; echo "st=$?"
diff -q bin1 bin2; echo "st=$?"
diff -a bin1 bin2 | od -c | head -3
diff f1 missing; echo "st=$?"
diff missing f1; echo "st=$?"
diff -N --label a --label b -u f1 missing; echo "st=$?"
diff -N missing f1; echo "st=$?"
diff f1; echo "st=$?"
diff f1 f2 f3; echo "st=$?"
diff -Q f1 f2; echo "st=$?"
diff -U x f1 f2; echo "st=$?"
diff - f2 < f1
diff f1 - < f2
diff d1 d2; echo "st=$?"
diff -r d1 d2; echo "st=$?"
diff -ru d1 d2 | grep -v '^[-+][-+][-+] '; echo "st=$?"
diff -rq d1 d2; echo "st=$?"
diff -rN d1 d2 | grep -v '^[-+][-+][-+] '; echo "st=$?"
diff -r -x 'only*' d1 d2; echo "st=$?"
diff d1 f1; echo "st=$?"
diff d1/common d2; echo "st=$?"
diff d1 d1; echo "st=$?"
@fresh seq 1 1000 > big1; seq 1 1000 | sed '500d;700s/$/x/' > big2; diff big1 big2; echo "st=$?"
@fresh seq 1 1000 > big1; seq 1 1000 | sed '500d;700s/$/x/' > big2; diff -u --label a --label b big1 big2; echo "st=$?"
@fresh seq 1 2000 > big1; seq 1 2000 | sed -n '1~3p' > big2; diff big1 big2 | wc -l
@fresh seq 1 300 > s1; seq 1 300 | sed 's/^/x/' > s2; diff s1 s2 | head -3; diff s1 s2 | wc -l
diff -u --label a --label b ab1 ab2
diff -T f1 f2
diff --unified=1 --label a --label b f1 f2
diff --context=0 --label a --label b f1 f2
diff --ignore-case case1 case2; echo "st=$?"
diff r0a r0b > /dev/null; echo st=$?; diff r0a r0b | grep -c '^<'; diff r0a r0b | grep -c '^>'
diff -U0 r0a r0b | grep -c '^-'; diff -U0 r0a r0b | grep -c '^+'
diff r1a r1b > /dev/null; echo st=$?; diff r1a r1b | grep -c '^<'; diff r1a r1b | grep -c '^>'
diff -U0 r1a r1b | grep -c '^-'; diff -U0 r1a r1b | grep -c '^+'
diff r2a r2b > /dev/null; echo st=$?; diff r2a r2b | grep -c '^<'; diff r2a r2b | grep -c '^>'
diff -U0 r2a r2b | grep -c '^-'; diff -U0 r2a r2b | grep -c '^+'
diff r3a r3b > /dev/null; echo st=$?; diff r3a r3b | grep -c '^<'; diff r3a r3b | grep -c '^>'
diff -U0 r3a r3b | grep -c '^-'; diff -U0 r3a r3b | grep -c '^+'
diff r4a r4b > /dev/null; echo st=$?; diff r4a r4b | grep -c '^<'; diff r4a r4b | grep -c '^>'
diff -U0 r4a r4b | grep -c '^-'; diff -U0 r4a r4b | grep -c '^+'
diff r5a r5b > /dev/null; echo st=$?; diff r5a r5b | grep -c '^<'; diff r5a r5b | grep -c '^>'
diff -U0 r5a r5b | grep -c '^-'; diff -U0 r5a r5b | grep -c '^+'
diff r6a r6b > /dev/null; echo st=$?; diff r6a r6b | grep -c '^<'; diff r6a r6b | grep -c '^>'
diff -U0 r6a r6b | grep -c '^-'; diff -U0 r6a r6b | grep -c '^+'
diff r7a r7b > /dev/null; echo st=$?; diff r7a r7b | grep -c '^<'; diff r7a r7b | grep -c '^>'
diff -U0 r7a r7b | grep -c '^-'; diff -U0 r7a r7b | grep -c '^+'
diff r8a r8b > /dev/null; echo st=$?; diff r8a r8b | grep -c '^<'; diff r8a r8b | grep -c '^>'
diff -U0 r8a r8b | grep -c '^-'; diff -U0 r8a r8b | grep -c '^+'
diff r9a r9b > /dev/null; echo st=$?; diff r9a r9b | grep -c '^<'; diff r9a r9b | grep -c '^>'
diff -U0 r9a r9b | grep -c '^-'; diff -U0 r9a r9b | grep -c '^+'
diff r10a r10b > /dev/null; echo st=$?; diff r10a r10b | grep -c '^<'; diff r10a r10b | grep -c '^>'
diff -U0 r10a r10b | grep -c '^-'; diff -U0 r10a r10b | grep -c '^+'
diff r11a r11b > /dev/null; echo st=$?; diff r11a r11b | grep -c '^<'; diff r11a r11b | grep -c '^>'
diff -U0 r11a r11b | grep -c '^-'; diff -U0 r11a r11b | grep -c '^+'
diff r12a r12b > /dev/null; echo st=$?; diff r12a r12b | grep -c '^<'; diff r12a r12b | grep -c '^>'
diff -U0 r12a r12b | grep -c '^-'; diff -U0 r12a r12b | grep -c '^+'
diff r13a r13b > /dev/null; echo st=$?; diff r13a r13b | grep -c '^<'; diff r13a r13b | grep -c '^>'
diff -U0 r13a r13b | grep -c '^-'; diff -U0 r13a r13b | grep -c '^+'
diff r14a r14b > /dev/null; echo st=$?; diff r14a r14b | grep -c '^<'; diff r14a r14b | grep -c '^>'
diff -U0 r14a r14b | grep -c '^-'; diff -U0 r14a r14b | grep -c '^+'
diff r15a r15b > /dev/null; echo st=$?; diff r15a r15b | grep -c '^<'; diff r15a r15b | grep -c '^>'
diff -U0 r15a r15b | grep -c '^-'; diff -U0 r15a r15b | grep -c '^+'
diff r16a r16b > /dev/null; echo st=$?; diff r16a r16b | grep -c '^<'; diff r16a r16b | grep -c '^>'
diff -U0 r16a r16b | grep -c '^-'; diff -U0 r16a r16b | grep -c '^+'
diff r17a r17b > /dev/null; echo st=$?; diff r17a r17b | grep -c '^<'; diff r17a r17b | grep -c '^>'
diff -U0 r17a r17b | grep -c '^-'; diff -U0 r17a r17b | grep -c '^+'
diff r18a r18b > /dev/null; echo st=$?; diff r18a r18b | grep -c '^<'; diff r18a r18b | grep -c '^>'
diff -U0 r18a r18b | grep -c '^-'; diff -U0 r18a r18b | grep -c '^+'
diff r19a r19b > /dev/null; echo st=$?; diff r19a r19b | grep -c '^<'; diff r19a r19b | grep -c '^>'
diff -U0 r19a r19b | grep -c '^-'; diff -U0 r19a r19b | grep -c '^+'
diff r20a r20b > /dev/null; echo st=$?; diff r20a r20b | grep -c '^<'; diff r20a r20b | grep -c '^>'
diff -U0 r20a r20b | grep -c '^-'; diff -U0 r20a r20b | grep -c '^+'
diff r21a r21b > /dev/null; echo st=$?; diff r21a r21b | grep -c '^<'; diff r21a r21b | grep -c '^>'
diff -U0 r21a r21b | grep -c '^-'; diff -U0 r21a r21b | grep -c '^+'
diff r22a r22b > /dev/null; echo st=$?; diff r22a r22b | grep -c '^<'; diff r22a r22b | grep -c '^>'
diff -U0 r22a r22b | grep -c '^-'; diff -U0 r22a r22b | grep -c '^+'
diff r23a r23b > /dev/null; echo st=$?; diff r23a r23b | grep -c '^<'; diff r23a r23b | grep -c '^>'
diff -U0 r23a r23b | grep -c '^-'; diff -U0 r23a r23b | grep -c '^+'
diff r24a r24b > /dev/null; echo st=$?; diff r24a r24b | grep -c '^<'; diff r24a r24b | grep -c '^>'
diff -U0 r24a r24b | grep -c '^-'; diff -U0 r24a r24b | grep -c '^+'
diff r25a r25b > /dev/null; echo st=$?; diff r25a r25b | grep -c '^<'; diff r25a r25b | grep -c '^>'
diff -U0 r25a r25b | grep -c '^-'; diff -U0 r25a r25b | grep -c '^+'
diff r26a r26b > /dev/null; echo st=$?; diff r26a r26b | grep -c '^<'; diff r26a r26b | grep -c '^>'
diff -U0 r26a r26b | grep -c '^-'; diff -U0 r26a r26b | grep -c '^+'
diff r27a r27b > /dev/null; echo st=$?; diff r27a r27b | grep -c '^<'; diff r27a r27b | grep -c '^>'
diff -U0 r27a r27b | grep -c '^-'; diff -U0 r27a r27b | grep -c '^+'
diff r28a r28b > /dev/null; echo st=$?; diff r28a r28b | grep -c '^<'; diff r28a r28b | grep -c '^>'
diff -U0 r28a r28b | grep -c '^-'; diff -U0 r28a r28b | grep -c '^+'
diff r29a r29b > /dev/null; echo st=$?; diff r29a r29b | grep -c '^<'; diff r29a r29b | grep -c '^>'
diff -U0 r29a r29b | grep -c '^-'; diff -U0 r29a r29b | grep -c '^+'
diff r30a r30b > /dev/null; echo st=$?; diff r30a r30b | grep -c '^<'; diff r30a r30b | grep -c '^>'
diff -U0 r30a r30b | grep -c '^-'; diff -U0 r30a r30b | grep -c '^+'
diff r31a r31b > /dev/null; echo st=$?; diff r31a r31b | grep -c '^<'; diff r31a r31b | grep -c '^>'
diff -U0 r31a r31b | grep -c '^-'; diff -U0 r31a r31b | grep -c '^+'
diff r32a r32b > /dev/null; echo st=$?; diff r32a r32b | grep -c '^<'; diff r32a r32b | grep -c '^>'
diff -U0 r32a r32b | grep -c '^-'; diff -U0 r32a r32b | grep -c '^+'
diff r33a r33b > /dev/null; echo st=$?; diff r33a r33b | grep -c '^<'; diff r33a r33b | grep -c '^>'
diff -U0 r33a r33b | grep -c '^-'; diff -U0 r33a r33b | grep -c '^+'
diff r34a r34b > /dev/null; echo st=$?; diff r34a r34b | grep -c '^<'; diff r34a r34b | grep -c '^>'
diff -U0 r34a r34b | grep -c '^-'; diff -U0 r34a r34b | grep -c '^+'
diff r35a r35b > /dev/null; echo st=$?; diff r35a r35b | grep -c '^<'; diff r35a r35b | grep -c '^>'
diff -U0 r35a r35b | grep -c '^-'; diff -U0 r35a r35b | grep -c '^+'
diff r36a r36b > /dev/null; echo st=$?; diff r36a r36b | grep -c '^<'; diff r36a r36b | grep -c '^>'
diff -U0 r36a r36b | grep -c '^-'; diff -U0 r36a r36b | grep -c '^+'
diff r37a r37b > /dev/null; echo st=$?; diff r37a r37b | grep -c '^<'; diff r37a r37b | grep -c '^>'
diff -U0 r37a r37b | grep -c '^-'; diff -U0 r37a r37b | grep -c '^+'
diff r38a r38b > /dev/null; echo st=$?; diff r38a r38b | grep -c '^<'; diff r38a r38b | grep -c '^>'
diff -U0 r38a r38b | grep -c '^-'; diff -U0 r38a r38b | grep -c '^+'
diff r39a r39b > /dev/null; echo st=$?; diff r39a r39b | grep -c '^<'; diff r39a r39b | grep -c '^>'
diff -U0 r39a r39b | grep -c '^-'; diff -U0 r39a r39b | grep -c '^+'
diff r40a r40b > /dev/null; echo st=$?; diff r40a r40b | grep -c '^<'; diff r40a r40b | grep -c '^>'
diff -U0 r40a r40b | grep -c '^-'; diff -U0 r40a r40b | grep -c '^+'
diff r41a r41b > /dev/null; echo st=$?; diff r41a r41b | grep -c '^<'; diff r41a r41b | grep -c '^>'
diff -U0 r41a r41b | grep -c '^-'; diff -U0 r41a r41b | grep -c '^+'
diff r42a r42b > /dev/null; echo st=$?; diff r42a r42b | grep -c '^<'; diff r42a r42b | grep -c '^>'
diff -U0 r42a r42b | grep -c '^-'; diff -U0 r42a r42b | grep -c '^+'
diff r43a r43b > /dev/null; echo st=$?; diff r43a r43b | grep -c '^<'; diff r43a r43b | grep -c '^>'
diff -U0 r43a r43b | grep -c '^-'; diff -U0 r43a r43b | grep -c '^+'
diff r44a r44b > /dev/null; echo st=$?; diff r44a r44b | grep -c '^<'; diff r44a r44b | grep -c '^>'
diff -U0 r44a r44b | grep -c '^-'; diff -U0 r44a r44b | grep -c '^+'
diff r45a r45b > /dev/null; echo st=$?; diff r45a r45b | grep -c '^<'; diff r45a r45b | grep -c '^>'
diff -U0 r45a r45b | grep -c '^-'; diff -U0 r45a r45b | grep -c '^+'
diff r46a r46b > /dev/null; echo st=$?; diff r46a r46b | grep -c '^<'; diff r46a r46b | grep -c '^>'
diff -U0 r46a r46b | grep -c '^-'; diff -U0 r46a r46b | grep -c '^+'
diff r47a r47b > /dev/null; echo st=$?; diff r47a r47b | grep -c '^<'; diff r47a r47b | grep -c '^>'
diff -U0 r47a r47b | grep -c '^-'; diff -U0 r47a r47b | grep -c '^+'
diff r48a r48b > /dev/null; echo st=$?; diff r48a r48b | grep -c '^<'; diff r48a r48b | grep -c '^>'
diff -U0 r48a r48b | grep -c '^-'; diff -U0 r48a r48b | grep -c '^+'
diff r49a r49b > /dev/null; echo st=$?; diff r49a r49b | grep -c '^<'; diff r49a r49b | grep -c '^>'
diff -U0 r49a r49b | grep -c '^-'; diff -U0 r49a r49b | grep -c '^+'
diff r50a r50b > /dev/null; echo st=$?; diff r50a r50b | grep -c '^<'; diff r50a r50b | grep -c '^>'
diff -U0 r50a r50b | grep -c '^-'; diff -U0 r50a r50b | grep -c '^+'
diff r51a r51b > /dev/null; echo st=$?; diff r51a r51b | grep -c '^<'; diff r51a r51b | grep -c '^>'
diff -U0 r51a r51b | grep -c '^-'; diff -U0 r51a r51b | grep -c '^+'
diff r52a r52b > /dev/null; echo st=$?; diff r52a r52b | grep -c '^<'; diff r52a r52b | grep -c '^>'
diff -U0 r52a r52b | grep -c '^-'; diff -U0 r52a r52b | grep -c '^+'
diff r53a r53b > /dev/null; echo st=$?; diff r53a r53b | grep -c '^<'; diff r53a r53b | grep -c '^>'
diff -U0 r53a r53b | grep -c '^-'; diff -U0 r53a r53b | grep -c '^+'
diff r54a r54b > /dev/null; echo st=$?; diff r54a r54b | grep -c '^<'; diff r54a r54b | grep -c '^>'
diff -U0 r54a r54b | grep -c '^-'; diff -U0 r54a r54b | grep -c '^+'
diff r55a r55b > /dev/null; echo st=$?; diff r55a r55b | grep -c '^<'; diff r55a r55b | grep -c '^>'
diff -U0 r55a r55b | grep -c '^-'; diff -U0 r55a r55b | grep -c '^+'
diff r56a r56b > /dev/null; echo st=$?; diff r56a r56b | grep -c '^<'; diff r56a r56b | grep -c '^>'
diff -U0 r56a r56b | grep -c '^-'; diff -U0 r56a r56b | grep -c '^+'
diff r57a r57b > /dev/null; echo st=$?; diff r57a r57b | grep -c '^<'; diff r57a r57b | grep -c '^>'
diff -U0 r57a r57b | grep -c '^-'; diff -U0 r57a r57b | grep -c '^+'
diff r58a r58b > /dev/null; echo st=$?; diff r58a r58b | grep -c '^<'; diff r58a r58b | grep -c '^>'
diff -U0 r58a r58b | grep -c '^-'; diff -U0 r58a r58b | grep -c '^+'
diff r59a r59b > /dev/null; echo st=$?; diff r59a r59b | grep -c '^<'; diff r59a r59b | grep -c '^>'
diff -U0 r59a r59b | grep -c '^-'; diff -U0 r59a r59b | grep -c '^+'
diff r60a r60b > /dev/null; echo st=$?; diff r60a r60b | grep -c '^<'; diff r60a r60b | grep -c '^>'
diff -U0 r60a r60b | grep -c '^-'; diff -U0 r60a r60b | grep -c '^+'
diff r61a r61b > /dev/null; echo st=$?; diff r61a r61b | grep -c '^<'; diff r61a r61b | grep -c '^>'
diff -U0 r61a r61b | grep -c '^-'; diff -U0 r61a r61b | grep -c '^+'
diff r62a r62b > /dev/null; echo st=$?; diff r62a r62b | grep -c '^<'; diff r62a r62b | grep -c '^>'
diff -U0 r62a r62b | grep -c '^-'; diff -U0 r62a r62b | grep -c '^+'
diff r63a r63b > /dev/null; echo st=$?; diff r63a r63b | grep -c '^<'; diff r63a r63b | grep -c '^>'
diff -U0 r63a r63b | grep -c '^-'; diff -U0 r63a r63b | grep -c '^+'
diff r64a r64b > /dev/null; echo st=$?; diff r64a r64b | grep -c '^<'; diff r64a r64b | grep -c '^>'
diff -U0 r64a r64b | grep -c '^-'; diff -U0 r64a r64b | grep -c '^+'
diff r65a r65b > /dev/null; echo st=$?; diff r65a r65b | grep -c '^<'; diff r65a r65b | grep -c '^>'
diff -U0 r65a r65b | grep -c '^-'; diff -U0 r65a r65b | grep -c '^+'
diff r66a r66b > /dev/null; echo st=$?; diff r66a r66b | grep -c '^<'; diff r66a r66b | grep -c '^>'
diff -U0 r66a r66b | grep -c '^-'; diff -U0 r66a r66b | grep -c '^+'
diff r67a r67b > /dev/null; echo st=$?; diff r67a r67b | grep -c '^<'; diff r67a r67b | grep -c '^>'
diff -U0 r67a r67b | grep -c '^-'; diff -U0 r67a r67b | grep -c '^+'
diff r68a r68b > /dev/null; echo st=$?; diff r68a r68b | grep -c '^<'; diff r68a r68b | grep -c '^>'
diff -U0 r68a r68b | grep -c '^-'; diff -U0 r68a r68b | grep -c '^+'
diff r69a r69b > /dev/null; echo st=$?; diff r69a r69b | grep -c '^<'; diff r69a r69b | grep -c '^>'
diff -U0 r69a r69b | grep -c '^-'; diff -U0 r69a r69b | grep -c '^+'
