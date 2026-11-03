#!/usr/bin/env python3
"""Generates the name dictionaries of the language guesser for both platforms.

Desktop: desktop/nicegram-desktop/Telegram/SourceFiles/core/ggtg_name_langs_data.h
Android: android/Telegram-GGTG/TMessagesProj/src/main/java/app/ggtg/GgtgNameLangsData.kt

Edit the lists below, then run: python3 tools/gen_name_langs.py

Tokens are lower case latin letters without diacritics. A token in "strong" says the language almost
for certain (1.0 point), "medium" is common but shared with other languages or with western names
(0.6), "weak" are short given-name pieces (0.3). A name is taken for a language at 1.0 point or more
and when it leads the second one clearly.
"""
import os

DICT = {
    "vi": {
        "strong": "nguyen tran pham hoang huynh phan truong dinh doan trinh luu quach thuy phuong thao nhung quyen "
                  "uyen khanh quynh nhi huong trang ngoc hieu cuong giang hanh hue lien thinh thang viet dung "
                  "tuan duc quang linh minh thanh",
        "medium": "le vu vo dang bui ngo duong ly lam hung hai binh son tam trung nam yen tien khoa loan nga oanh "
                  "phuc sang tai tri quoc xuan",
        "weak": "van thi anh lan mai ha ho do kim long hoa tu vinh nhan phu my",
    },
    "ko": {
        "strong": "minjun seojun junseo jiwoo hyunwoo eunji seoyeon jiho jihoon minseo seoyun hayoon jiyoung "
                  "sooyoung yejin dahye chaewon sunghoon donghyun taehyun jaehyun seungwoo youngho sangwoo "
                  "minhyuk junho jaewon dongwook kwon hwang jeong jeon yoon byun baek seong kwak",
        "medium": "kim lee park choi jung kang cho jang lim han oh seo shin ahn song yoo hong ko moon yang son bae "
                  "heo nam sim noh ryu yeo gu bang",
        "weak": "min jun ji hyun soo young jin eun hye sung woo joon hee yeon mi ae kyung dong chul ho sun ja ok bin "
                "yu ra na do hyo tae",
    },
    "ja": {
        "strong": "sato suzuki takahashi tanaka watanabe ito yamamoto nakamura kobayashi kato yoshida yamada sasaki "
                  "yamaguchi matsumoto inoue kimura hayashi shimizu yamazaki ikeda hashimoto ishikawa ogawa okada "
                  "hasegawa fujita maeda kondo murakami endo aoki sakamoto fujii nishimura fukuda miura okamoto "
                  "matsuda nakajima harada takeuchi nakagawa haruto yuto sota riku hiroshi takeshi kenji yuta aoi "
                  "yui sakura yuna mio himari akira daiki kazuki shota ryota naoki satoshi takumi tomoya masaki "
                  "koji kenta shinji hideo kenichi kaori naoko yumi keiko tomoko ayumi hiromi megumi sayuri miyuki",
        "medium": "ren yuki hana rin mori abe ono ota goto saito emi",
        "weak": "mai yu ai",
    },
    "zh": {
        "strong": "zhang zhao zhou zhu zheng zhong xu xie xiao xue jiang qin cheng huang liu chen wang yang wu sun "
                  "guo gao luo liang tang deng feng zeng peng cai pan tian dong yuan xiaoming xiaohong xiaoyan "
                  "jingjing weiwei lili mingming",
        "medium": "li lin ma han yu he lu wei hu fan fu shen yan ye ding jia ren xiang shi wan duan",
        "weak": "jun yong lei qiang jie ming hua xin yi hao yue ting fang jing na",
    },
    "id": {
        "strong": "budi sari dewi putra putri wulan agus siti rizky dian eko hendra wahyu fitri aditya andi ayu indah "
                  "setiawan wijaya kurniawan santoso saputra hidayat pratama utama rahman lestari purnama wati "
                  "yanti hartono susanto suharto gunawan permana",
        "medium": "ahmad muhammad dwi tri sri ratna sinta joko bambang slamet nur",
        "weak": "putra",
    },
    "th": {
        "strong": "somchai somsak somporn suda nattapong chaiyaporn kanya pimchanok pichit siriporn anan boonmee "
                  "sirikul ploy sakda wichai pornthip suchart sunan thanakorn kittipong apinya natthaphon chanida "
                  "napat panya phakin sarawut supachai thitiwat wanida jirapat kritsada prasert pattarapon "
                  "sompong yupa",
        "medium": "nok mali",
        "weak": "",
    },
    "tr": {
        "strong": "mehmet mustafa ahmet hasan huseyin yilmaz kaya demir sahin celik yildiz ozturk arslan dogan "
                  "aydin kilic aslan ayse fatma emre burak murat elif zeynep cem ozkan kaplan polat koc korkmaz "
                  "ozdemir erdogan yildirim ozer gunes bulut tekin cetin kurt",
        "medium": "ali can deniz",
        "weak": "",
    },
    "es": {
        "strong": "garcia rodriguez gonzalez fernandez lopez martinez sanchez perez gomez diaz ruiz hernandez "
                  "jimenez alvarez moreno munoz romero navarro torres dominguez vazquez ramos serrano blanco "
                  "molina morales ortega delgado ortiz rubio marin iglesias medina cortes garrido castillo "
                  "carlos jose juan miguel pedro javier lucia carmen pablo alejandro manuel francisco antonio "
                  "rafael sergio andres fernando diego ramon raul mateo valentina camila isabella",
        "medium": "maria luis sofia gil santos castro",
        "weak": "",
    },
    "pt": {
        "strong": "silva oliveira souza costa pereira ferreira rodrigues almeida nascimento lima araujo ribeiro "
                  "carvalho gomes martins rocha barbosa moreira cavalcante teixeira cardoso correia mendes nunes "
                  "soares vieira monteiro freitas joao thiago tiago mariana beatriz larissa luana fernanda "
                  "juliana vitor guilherme caio matheus",
        "medium": "ana lucas gabriel felipe bruno",
        "weak": "",
    },
    "ar": {
        "strong": "mohammed muhammad mohamed ahmed omar umar khaled khalid hassan hussein hussain abdullah "
                  "abdallah abdul abdelrahman ibrahim youssef yousef yusuf mahmoud fatima aisha zainab maryam "
                  "alharbi alqahtani almutairi alotaibi alshehri alghamdi aldosari hamdan layla leila amina",
        "medium": "abu bin ibn al el noor nour sara ali",
        "weak": "",
    },
    "hi": {
        "strong": "sharma singh kumar patel gupta verma yadav mishra reddy rao nair iyer joshi mehta kapoor chopra "
                  "malhotra khanna agarwal bansal jain desai pandey tiwari chauhan thakur rahul amit priya neha "
                  "pooja anil sunil vijay ravi deepak rohit ankit sanjay rajesh suresh manish arjun aditi ananya "
                  "divya kavita sneha swati",
        "medium": "das shah khan",
        "weak": "",
    },
    "de": {
        "strong": "mueller muller schmidt schneider fischer weber meyer wagner becker schulz hoffmann schaefer "
                  "bauer richter schroeder neumann schwarz zimmermann krueger hofmann hartmann schmitt werner "
                  "krause lehmann klaus jurgen wolfgang dieter helmut gunter heinz ursula monika sabine anja "
                  "katrin lukas jonas felix maximilian",
        "medium": "hans koch klein wolf braun lange petra",
        "weak": "",
    },
    "fr": {
        "strong": "dubois durand moreau lefebvre leroy garnier bonnet dupont fontaine rousseau mercier lambert "
                  "fournier girard gauthier lefevre chevalier perrin morel fabre roux francois sebastien "
                  "guillaume etienne mathieu benoit amelie",
        "medium": "jean pierre michel nicolas camille julien antoine laurent",
        "weak": "",
    },
    "it": {
        "strong": "rossi russo ferrari esposito bianchi romano colombo ricci marino greco bruno gallo conti "
                  "mancini giordano rizzo lombardi moretti barbieri fontana santoro mariani rinaldi giovanni "
                  "marco luca francesco giuseppe giorgio alessandro andrea matteo lorenzo giulia chiara "
                  "francesca elena",
        "medium": "antonio",
        "weak": "",
    },
    "pl": {
        "strong": "kowalski nowak wisniewski wojcik kowalczyk kaminski lewandowski zielinski szymanski wozniak "
                  "dabrowski kozlowski jankowski mazur kwiatkowski andrzej piotr krzysztof tomasz pawel michal "
                  "marcin agnieszka katarzyna malgorzata",
        "medium": "jan",
        "weak": "",
    },
    "ru": {
        "strong": "ivanov petrov sidorov smirnov kuznetsov popov vasiliev vasilyev sokolov mikhailov novikov fedorov "
                  "morozov volkov alexeev alekseev lebedev semenov egorov pavlov kozlov stepanov nikolaev orlov "
                  "andreev makarov nikitin zakharov zaitsev solovyov borisov yakovlev grigoriev romanov vorobyov "
                  "sergeev kuzmin frolov aleksandrov dmitriev korolev gusev kiselyov ilyin maksimov polyakov "
                  "sorokin vinogradov kovalev belov medvedev antonov tarasov zhukov baranov filippov komarov "
                  "davydov belyaev gerasimov bogdanov osipov aleksandr sergey sergei dmitry dmitri "
                  "andrey andrei alexey aleksey maksim nikolay nikolai vladimir vladislav evgeny yevgeny "
                  "mikhail artem vitaly natalya tatyana svetlana "
                  "ekaterina yulia ludmila galina",
        "medium": "ivan pavel anton alexander maxim oleg igor viktor olga natalia irina tatiana daria anastasia",
        "weak": "",
    },
    "uk": {
        "strong": "shevchenko kovalenko bondarenko tkachenko kovalchuk kravchenko oliynyk shevchuk polishchuk "
                  "moroz melnyk boyko tkachuk lysenko marchenko savchenko rudenko petrenko klymenko",
        "medium": "",
        "weak": "",
    },
}

# A token that ends like this is taken for a surname of the language (0.8 point, 5 letters or more).
SUFFIX = {
    "ru": "ov ev ova eva ovich evich ovna evna skiy sky skaya skaja yev yova",
    "uk": "enko chuk yuk ovych ivna",
}

# Words of a group / base title that say where its people are from (regular expression pieces,
# lower case). Two-letter codes are matched as whole words only.
TITLES = {
    "ko": "коре|korea|한국|seoul|сеул",
    "zh": "кита|china|chinese|中国|中文|beijing|пекин|shanghai|шанхай",
    "ja": "япон|japan|日本|tokyo|токио",
    "vi": "вьетнам|vietnam|việt|hanoi|ханой|сайгон|saigon",
    "th": "таиланд|тайланд|thailand|ไทย|bangkok|бангкок|пхукет|phuket|паттай|pattaya",
    "id": "индонез|indonesia|bali|бали|jakarta|джакарта",
    "tr": "турци|турец|turkey|türkiye|turkiye|istanbul|стамбул|анталь|antalya",
    "hi": "инди|india|hindi|mumbai|delhi|мумбаи",
    "ar": "араб|arab|эмират|dubai|дубай|saudi|саудов|египет|egypt|марокк|morocco",
    "es": "испан|spain|españa|espana|latam|mexico|мексик|argentin|аргентин|colombia|колумб",
    "pt": "португал|portugal|brasil|brazil|бразил",
    "de": "герман|germany|deutsch|berlin|берлин",
    "fr": "франц|france|français|paris|париж",
    "it": "итал|italy|italia|roma|рим",
    "pl": "польш|poland|polska|warszawa|варшав",
}
TITLE_WORDS = {
    "ko": "kr",
    "zh": "cn",
    "ja": "jp",
    "vi": "vn",
    "tr": "tr",
    "id": "id",
}

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CPP = os.path.join(ROOT, "desktop/nicegram-desktop/Telegram/SourceFiles/core/ggtg_name_langs_data.h")
KT = os.path.join(ROOT, "android/Telegram-GGTG/TMessagesProj/src/main/java/app/ggtg/GgtgNameLangsData.kt")


def lines(text, width=100):
    out, cur = [], ""
    for word in text.split():
        if len(cur) + len(word) + 1 > width:
            out.append(cur)
            cur = word
        else:
            cur = (cur + " " + word).strip()
    if cur:
        out.append(cur)
    return out


def cpp_str(text):
    parts = lines(text)
    if not parts:
        return '""'
    return "\n\t\t\t".join('"%s%s"' % (p, " " if i < len(parts) - 1 else "") for i, p in enumerate(parts))


def kt_str(text):
    parts = lines(text)
    if not parts:
        return '""'
    return " +\n            ".join('"%s%s"' % (p, " " if i < len(parts) - 1 else "") for i, p in enumerate(parts))


def main():
    cpp = ["// Generated by tools/gen_name_langs.py: do not edit by hand.", "#pragma once", "",
           "#include <vector>", "", "namespace Core::GgtgLang::details {", "",
           "struct NameDictionary {", "\tconst char *code;", "\tconst char *strong;", "\tconst char *medium;",
           "\tconst char *weak;", "};", "",
           "[[nodiscard]] inline const std::vector<NameDictionary> &NameDictionaries() {",
           "\tstatic const auto result = std::vector<NameDictionary>{"]
    for code, d in DICT.items():
        cpp.append("\t\t{ \"%s\"," % code)
        cpp.append("\t\t\t%s," % cpp_str(d["strong"]))
        cpp.append("\t\t\t%s," % cpp_str(d["medium"]))
        cpp.append("\t\t\t%s }," % cpp_str(d["weak"]))
    cpp += ["\t};", "\treturn result;", "}", "",
            "struct NameSuffixes {", "\tconst char *code;", "\tconst char *suffixes;", "};", "",
            "[[nodiscard]] inline const std::vector<NameSuffixes> &NameSuffixList() {",
            "\tstatic const auto result = std::vector<NameSuffixes>{"]
    for code, text in SUFFIX.items():
        cpp.append('\t\t{ "%s", "%s" },' % (code, text))
    cpp += ["\t};", "\treturn result;", "}", "",
            "struct TitleHints {", "\tconst char *code;", "\tconst char *pattern;", "\tconst char *word;", "};", "",
            "[[nodiscard]] inline const std::vector<TitleHints> &TitleHintList() {",
            "\tstatic const auto result = std::vector<TitleHints>{"]
    for code, pattern in TITLES.items():
        cpp.append('\t\t{ "%s", "%s", "%s" },' % (code, pattern, TITLE_WORDS.get(code, "")))
    cpp += ["\t};", "\treturn result;", "}", "", "} // namespace Core::GgtgLang::details", ""]
    with open(CPP, "w", encoding="utf-8") as f:
        f.write("\n".join(cpp))

    kt = ["package app.ggtg", "", "// Generated by tools/gen_name_langs.py: do not edit by hand.",
          "object GgtgNameLangsData {", "    // code -> (strong, medium, weak) tokens, space separated",
          "    val dictionaries: Map<String, Triple<String, String, String>> = mapOf("]
    for code, d in DICT.items():
        kt.append('        "%s" to Triple(' % code)
        kt.append("            %s," % kt_str(d["strong"]))
        kt.append("            %s," % kt_str(d["medium"]))
        kt.append("            %s)," % kt_str(d["weak"]))
    kt += ["    )", "", "    val suffixes: Map<String, String> = mapOf("]
    for code, text in SUFFIX.items():
        kt.append('        "%s" to "%s",' % (code, text))
    kt += ["    )", "", "    // code -> (regular expression of the title words, a short whole word like \"kr\" or \"\")",
           "    val titles: Map<String, Pair<String, String>> = mapOf("]
    for code, pattern in TITLES.items():
        kt.append('        "%s" to Pair("%s", "%s"),' % (code, pattern, TITLE_WORDS.get(code, "")))
    kt += ["    )", "}", ""]
    with open(KT, "w", encoding="utf-8") as f:
        f.write("\n".join(kt))
    print("ok")


main()
