package com.mhgu.recomp;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * The app's own screens (game folder, drivers, imports, saves, log) in Chinese (Simplified) when the phone is set to
 * Chinese. They are written in English and Spanish; with a Chinese phone they pick the English, and every text they
 * show goes through {@link #t}: the whole text if it is here, else each known piece of it (texts made of several
 * pieces with a name or a number between them). Anything not here stays English.
 * The settings menu has its own table (tools/language_zh*.tsv).
 */
final class Zh {
    private Zh() {}

    static boolean on() {
        return "zh".equals(Locale.getDefault().getLanguage());
    }

    private static final Map<String, String> ALL = new HashMap<>();
    private static final List<String> PIECES = new ArrayList<>();  // (longest first)

    private static void put(String en, String zh) {
        ALL.put(en, zh);
    }

    static CharSequence t(CharSequence s) {
        return s instanceof String ? t((String) s) : s;
    }

    static String t(String s) {
        if (s == null || s.isEmpty() || !on()) return s;
        String whole = ALL.get(s);
        if (whole != null) return whole;
        String out = s;
        for (String k : PIECES)
            if (out.contains(k)) out = out.replace(k, ALL.get(k));
        return out;
    }

    static {
        // ---- game folder
        put("The folder ", "文件夹 ");
        put(" is gone. Choose where it is now.", " 已不存在。请选择它现在的位置。");
        put("The game lost its permission to read the folder. Please give it again.", "游戏失去了读取该文件夹的权限。请重新授予。");
        put("Your folder", "你的文件夹");
        put("│    ... 35,500 files", "│    ... 35,500 个文件");
        put("GAME FILES", "游戏文件");
        put("Choose the folder that holds your copy of the game. It is read from there: nothing is copied.",
            "选择存放你的游戏副本的文件夹。游戏直接从那里读取：不会复制任何内容。");
        put("THE FOLDER MUST LOOK LIKE THIS", "文件夹必须是这样的");
        put("WHAT EACH ONE MUST BE", "每一项必须是什么");
        put("• exefs: the one from the 1.4.0 update.\n", "• exefs：来自 1.4.0 更新的 exefs。\n");
        put("• romfs: the base game's, with the 1.4.0 update already applied over it.\n", "• romfs：本体游戏的 romfs，并已叠加 1.4.0 更新。\n");
        put("• libmhgugame.so: made once on a PC with the guide's tool.\n", "• libmhgugame.so：在电脑上用指南中的工具生成一次。\n");
        put("• All in the same folder, about 12 GB. Choose that folder.", "• 全部放在同一个文件夹中，约 12 GB。选择那个文件夹。");
        put("Cancel", "取消");
        put("Exit", "退出");
        put("Choose folder", "选择文件夹");
        put("Give permission and choose folder", "授予权限并选择文件夹");
        put("Turn on access to all files and come back. The game only reads that folder: it changes nothing.",
            "请开启\"所有文件访问权限\"后返回。游戏只读取那个文件夹：不会更改任何内容。");
        put("This device has no folder picker.", "此设备没有文件夹选择器。");
        put("CHOOSE THE GAME'S FOLDER", "选择游戏文件夹");
        put("Phone storage", "手机存储");
        put("SD card", "SD 卡");
        put("✓ The game is here: exefs and romfs.", "✓ 游戏在这里：exefs 和 romfs。");
        put("Go into the folder that holds exefs and romfs.", "进入包含 exefs 和 romfs 的文件夹。");
        put("Up", "上一级");
        put("(no folders)", "（没有文件夹）");
        put("Use this folder", "使用此文件夹");
        put("Android's picker", "Android 选择器");
        put("That location can't be used. Find the folder in the phone's storage or on the SD card.",
            "无法使用该位置。请在手机存储或 SD 卡中找到该文件夹。");
        put("Checking the files...", "正在检查文件...");
        put("exefs and romfs are not in ", "exefs 和 romfs 不在 ");
        put(". Choose the folder that holds them.", " 中。请选择包含它们的文件夹。");
        put("The exefs folder is missing, beside romfs.", "romfs 旁边缺少 exefs 文件夹。");
        put("The romfs folder is missing, beside exefs.", "exefs 旁边缺少 romfs 文件夹。");
        put("The file main is missing from exefs.", "exefs 中缺少 main 文件。");
        put("exefs/main could not be read.", "无法读取 exefs/main。");
        put("This exefs is not version 1.4.0. Use the one from the 1.4.0 update.", "此 exefs 不是 1.4.0 版本。请使用 1.4.0 更新中的 exefs。");
        put("romfs is incomplete: it has ", "romfs 不完整：有 ");
        put(" files where there should be 35,531. Use the base game with the 1.4.0 update applied.",
            " 个文件，应为 35,531 个。请使用已叠加 1.4.0 更新的本体游戏。");
        put("libmhgugame.so is missing from that folder. Make it on a PC with the guide's tool.",
            "该文件夹中缺少 libmhgugame.so。请在电脑上用指南中的工具生成。");
        put("This libmhgugame.so is from another version. Make it again with this version's tool.",
            "此 libmhgugame.so 来自其他版本。请用此版本的工具重新生成。");
        put("nativeNX/system/app_shader/AppShaderPackage.mfx is missing from romfs. Use the base game with the 1.4.0 update applied.",
            "romfs 中缺少 nativeNX/system/app_shader/AppShaderPackage.mfx。请使用已叠加 1.4.0 更新的本体游戏。");
        put("Done: it will be used the next time you open the game.", "完成：下次打开游戏时将使用它。");

        // ---- GPU driver
        put("GPU DRIVER", "GPU 驱动");
        put("For Adreno GPUs only, at your own risk: a driver that isn't right for your GPU can close the game or look wrong. ",
            "仅限 Adreno GPU，风险自负：不适合你的 GPU 的驱动可能导致游戏关闭或画面异常。");
        put("If it fails at start, the app goes back to the system's driver by itself. Add a .zip with meta.json and the library.",
            "如果启动时失败，应用会自动恢复使用系统驱动。请添加包含 meta.json 和驱动库的 .zip。");
        put("Add a driver", "添加驱动");
        put("Close", "关闭");
        put("It didn't work: ", "未能生效：");
        put(". The system's is in use.", "。正在使用系统驱动。");
        put("The system's driver", "系统驱动");
        put("Remove", "移除");
        put("Could not save it: ", "无法保存：");
        put("Restart the game", "重启游戏");
        put("It applies when the game restarts. Save your game first.", "重启游戏后生效。请先保存游戏进度。");
        put("Installing…", "正在安装…");
        put("Installed: ", "已安装：");
        put(". Tap it to use it.", "。点按即可使用。");
        put("Could not install it: ", "无法安装：");
        put("too large", "文件太大");
        put("the zip has no .so library", "zip 中没有 .so 库");
        put("not a 64-bit ARM library", "不是 64 位 ARM 库");

        // ---- cheats, mods, DLC
        put("CHEATS", "作弊");
        put("MODS", "模组");
        put("Cheat .txt files in Atmosphere's format, for version 1.4.0 (its build id is FB08F1D20FD1204F). Then turn them on "
                + "one by one on the Cheats page. Those that change the game's code work, but that part of the game runs slower.",
            "Atmosphere 格式的作弊 .txt 文件，适用于 1.4.0 版本（构建 ID 为 FB08F1D20FD1204F）。之后在\"作弊\"页面逐个开启。"
                + "修改游戏代码的作弊可以使用，但游戏的那部分会运行得更慢。");
        put("A mod's folder with romfs or exefs in it, or a .zip of that folder. Then order and turn them on on the Mods page. "
                + "Code patches (exefs) work, but that part of the game runs slower.",
            "包含 romfs 或 exefs 的模组文件夹，或该文件夹的 .zip。之后在\"模组\"页面排序并开启。代码补丁（exefs）可以使用，但游戏的那部分会运行得更慢。");
        put("Your own DLC of the game: each one's folder holds a DLC.dlcp file. Choose a folder and every one in it is copied. "
                + "Then, in the game: main menu, Download. It is experimental.",
            "你自己的游戏 DLC：每个 DLC 的文件夹里有一个 DLC.dlcp 文件。选择一个文件夹，其中的所有 DLC 都会被复制。"
                + "然后在游戏中：主菜单，下载。此功能为实验性。");
        put("You can also copy them yourself into this folder on the phone: ", "你也可以自己把它们复制到手机上的这个文件夹：");
        put("Add cheats", "添加作弊");
        put("Add a mod", "添加模组");
        put("Add DLC", "添加 DLC");
        put("(nothing installed yet)", "（尚未安装任何内容）");
        put("CHOOSE A CHEAT FILE", "选择作弊文件");
        put("CHOOSE THE MOD'S FOLDER OR ITS .ZIP", "选择模组文件夹或其 .ZIP");
        put("CHOOSE YOUR DLC'S FOLDER", "选择你的 DLC 文件夹");
        put("(empty)", "（空）");
        put("Copying…", "正在复制…");
        put("Copying… ", "正在复制… ");
        put("Done: ", "完成：");
        put(". It applies when the game restarts.", "。重启游戏后生效。");
        put("It couldn't be done: ", "无法完成：");
        put("the .zip has no romfs or exefs", ".zip 中没有 romfs 或 exefs");
        put("that folder has no romfs or exefs in it", "该文件夹中没有 romfs 或 exefs");
        put("there is no DLC.dlcp in that folder", "该文件夹中没有 DLC.dlcp");
        put(" DLC copied", " 个 DLC 已复制");

        // ---- licenses, log
        put("LICENSES", "许可证");
        put("The game stopped because of an error", "游戏因错误而停止");
        put("Share log", "分享日志");
        put("The log says what happened and on which phone. You can send it to whoever is helping "
                + "you: the list of apps opens and you choose where it goes. Your save is not in it, but "
                + "it can hold your local network address if you played over LAN.",
            "日志记录了发生了什么以及在哪台手机上。你可以把它发给正在帮助你的人：会打开应用列表，由你选择发送到哪里。"
                + "日志中不包含你的存档，但如果你玩过局域网联机，可能包含你的本地网络地址。");

        // ---- saves
        put("EXPORT YOUR SAVE", "导出存档");
        put("Choose a folder and a new one is made there with your files system and system_backup. It copies what you last saved in the game.\n\n",
            "选择一个文件夹，会在其中新建一个文件夹，放入你的 system 和 system_backup 文件。复制的是你在游戏中最后一次保存的内容。\n\n");
        put("To use it on an emulator, move it to your PC and copy both files to the game's save folder:\n",
            "要在模拟器上使用，请把它移到电脑上，并将两个文件复制到游戏的存档文件夹：\n");
        put("• Ryujinx: right-click the game, Open User Save Directory.\n", "• Ryujinx：右键点击游戏，选择 Open User Save Directory。\n");
        put("• yuzu: right-click the game, Open Save Data Location.\n", "• yuzu：右键点击游戏，选择 Open Save Data Location。\n");
        put("Keep a copy of what is there first.", "请先备份那里已有的内容。");
        put("Choose a folder", "选择文件夹");
        put("There is no saved game yet.", "还没有任何存档。");
        put("Done: folder ", "完成：文件夹 ");
        put("Could not copy it: ", "无法复制：");
        put("that folder takes no new files", "该文件夹不接受新文件");
        put("IMPORT A SAVE", "导入存档");
        put("Bring your save from an emulator or another device.", "从模拟器或其他设备导入你的存档。");
        put("WHICH FILE TO CHOOSE", "选择哪个文件");
        put("It is the file named system, about 5 MB.\n", "是名为 system 的文件，约 5 MB。\n");
        put("Copy it here, for example to Downloads.", "把它复制到这台设备上，例如\"下载\"文件夹。");
        put("WHAT WILL HAPPEN", "将会发生什么");
        put("• All the characters on this device are replaced.\n", "• 此设备上的所有角色都会被替换。\n");
        put("• Your current save is kept in a copy, in MHGURecomp/save_backups.\n", "• 你当前的存档会备份到 MHGURecomp/save_backups。\n");
        put("• The game restarts: save first if you have progress to keep.", "• 游戏会重启：如有需要保留的进度，请先保存。");
        put("Choose file", "选择文件");
        put("Checking the save...", "正在检查存档...");
        put("The save is good. Going on replaces the one on this device.", "存档有效。继续将替换此设备上的存档。");
        put("Import and restart", "导入并重启");
        put("This device has no file picker.", "此设备没有文件选择器。");
        put("The app's folder could not be prepared.", "无法准备应用的文件夹。");
        put("That file could not be read. Copy it to the device's storage and try again.", "无法读取该文件。请把它复制到设备存储后重试。");
        put("That file is not a save of this game. Choose the file named system.", "该文件不是本游戏的存档。请选择名为 system 的文件。");
        put("The save could not be prepared. Nothing was changed.", "无法准备存档。没有做任何更改。");
        put("Restarting...", "正在重启...");

        // pieces: texts long enough (or ending as a label does) not to match inside a word
        for (String k : ALL.keySet())
            if ((k.length() >= 6 && k.contains(" ")) || k.endsWith(": ") || k.startsWith(". ") || k.contains("\n")) PIECES.add(k);
        Collections.sort(PIECES, (a, b) -> b.length() - a.length());
    }
}
