// ParalivesUpscaling - translations for our options, in the languages the game ships.
//
// Every label, dropdown entry, header and tooltip of FsrOptions is a "ParalivesUpscaling_*" key. The game resolves all of
// them through TranslationManager.Get, so a prefix there answers our keys for the game's current language (English
// fallback) and the options follow the language setting like the game's own labels.
using System;
using System.Collections.Generic;
using HarmonyLib;

namespace ParalivesUpscaling
{
    internal static class Localization
    {
        private const string Prefix = "ParalivesUpscaling_";

        // key (without prefix) -> en, es, pt, fr, de, it, pl, zh
        private static readonly string[] Locales = { "en", "es", "pt", "fr", "de", "it", "pl", "zh" };

        private static readonly Dictionary<string, string[]> Table = new Dictionary<string, string[]>
        {
            ["UpscalerVersion"] = new[] { "Upscaling Technology", "Tecnología de escalado", "Tecnologia de upscaling", "Technologie de mise à l'échelle", "Upscaling-Technologie", "Tecnologia di upscaling", "Technologia skalowania", "超分辨率技术" },
            ["Upscaler"] = new[] { "Upscaling Quality", "Calidad de escalado", "Qualidade do upscaling", "Qualité de mise à l'échelle", "Upscaling-Qualität", "Qualità upscaling", "Jakość skalowania", "超分辨率质量" },
            ["UpscalerSharpness"] = new[] { "Upscaling Sharpness", "Nitidez del escalado", "Nitidez do upscaling", "Netteté de la mise à l'échelle", "Upscaling-Schärfe", "Nitidezza upscaling", "Ostrość skalowania", "超分辨率锐化" },

            ["FrameGeneration"] = new[] { "Frame Generation", "Generación de cuadros", "Geração de quadros", "Génération d'images", "Frame Generation", "Generazione di fotogrammi", "Generowanie klatek", "帧生成" },
            ["FrameGen_Unavailable"] = new[] { "unavailable, see the BepInEx log", "no disponible, ver el log de BepInEx", "indisponível, veja o log do BepInEx", "indisponible, voir le journal BepInEx", "nicht verfügbar, siehe BepInEx-Log", "non disponibile, vedi il log di BepInEx", "niedostępne, zobacz log BepInEx", "不可用，请查看 BepInEx 日志" },
            ["FrameGen_NeedsUpscaling"] = new[] { "needs an upscaling technology", "necesita una tecnología de escalado", "precisa de uma tecnologia de upscaling", "nécessite une technologie de mise à l'échelle", "benötigt eine Upscaling-Technologie", "richiede una tecnologia di upscaling", "wymaga technologii skalowania", "需要开启超分辨率技术" },
            ["Tip_FrameGeneration"] = new[]
            {
                "Inserts a generated frame between every two rendered frames for smoother motion. Needs an upscaling technology switched on (Native AA is fine). Adds a little input latency. Switching between FSR and XeSS takes effect after restarting the game.",
                "Inserta un cuadro generado entre cada dos cuadros renderizados para un movimiento más fluido. Necesita una tecnología de escalado activada (AA nativo sirve). Agrega un poco de latencia. El cambio entre FSR y XeSS se aplica al reiniciar el juego.",
                "Insere um quadro gerado entre cada dois quadros renderizados para um movimento mais suave. Precisa de uma tecnologia de upscaling ativada (AA nativo serve). Adiciona um pouco de latência. A troca entre FSR e XeSS vale depois de reiniciar o jogo.",
                "Insère une image générée entre deux images rendues pour un mouvement plus fluide. Nécessite une technologie de mise à l'échelle activée (l'AA natif suffit). Ajoute un peu de latence. Le passage entre FSR et XeSS prend effet au redémarrage du jeu.",
                "Fügt zwischen je zwei gerenderten Bildern ein generiertes Bild ein, für flüssigere Bewegung. Benötigt eine aktive Upscaling-Technologie (Natives AA genügt). Erhöht die Eingabelatenz etwas. Ein Wechsel zwischen FSR und XeSS wirkt nach einem Neustart des Spiels.",
                "Inserisce un fotogramma generato tra due fotogrammi renderizzati per un movimento più fluido. Richiede una tecnologia di upscaling attiva (l'AA nativo va bene). Aggiunge un po' di latenza. Il passaggio tra FSR e XeSS ha effetto dopo il riavvio del gioco.",
                "Wstawia wygenerowaną klatkę między każde dwie renderowane klatki, dla płynniejszego ruchu. Wymaga włączonej technologii skalowania (natywny AA wystarczy). Dodaje nieco opóźnienia. Zmiana między FSR a XeSS działa po ponownym uruchomieniu gry.",
                "在每两帧渲染画面之间插入一帧生成画面，使运动更流畅。需要开启一种超分辨率技术（原生抗锯齿即可）。会略微增加输入延迟。在 FSR 和 XeSS 之间切换需重启游戏后生效。"
            },
            ["UpscalerShowInfo"] = new[] { "Show upscaler info on screen", "Mostrar info del escalado en pantalla", "Mostrar info do upscaling na tela", "Afficher les infos de mise à l'échelle", "Upscaling-Info auf dem Bildschirm", "Mostra info upscaling a schermo", "Pokaż informacje o skalowaniu", "在屏幕上显示超分辨率信息" },
            ["Tip_UpscalerShowInfo"] = new[]
            {
                "Shows at the top of the screen which upscaler is actually running (FSR 3, FSR 4, XeSS or DLSS) and its resolutions.",
                "Muestra arriba de la pantalla qué escalador se está usando realmente (FSR 3, FSR 4, XeSS o DLSS) y sus resoluciones.",
                "Mostra no topo da tela qual upscaler está realmente em uso (FSR 3, FSR 4, XeSS ou DLSS) e suas resoluções.",
                "Affiche en haut de l'écran la technologie réellement utilisée (FSR 3, FSR 4, XeSS ou DLSS) et ses résolutions.",
                "Zeigt oben auf dem Bildschirm, welches Upscaling tatsächlich läuft (FSR 3, FSR 4, XeSS oder DLSS) und seine Auflösungen.",
                "Mostra in alto sullo schermo quale upscaler è realmente in uso (FSR 3, FSR 4, XeSS o DLSS) e le sue risoluzioni.",
                "Pokazuje u góry ekranu, która technologia skalowania faktycznie działa (FSR 3, FSR 4, XeSS lub DLSS) i jej rozdzielczości.",
                "在屏幕顶部显示实际运行的超分辨率技术（FSR 3、FSR 4、XeSS 或 DLSS）及其分辨率。"
            },
            ["Status_Starting"] = new[] { "Upscaling: starting…", "Escalado: iniciando…", "Upscaling: iniciando…", "Mise à l'échelle : démarrage…", "Hochskalierung: startet…", "Upscaling: avvio…", "Skalowanie: uruchamianie…", "超分辨率：正在启动…" },
            ["Status_Unavailable"] = new[] { "Upscaling unavailable (see BepInEx log)", "Escalado no disponible (ver log de BepInEx)", "Upscaling indisponível (veja o log do BepInEx)", "Mise à l'échelle indisponible (voir le journal BepInEx)", "Hochskalierung nicht verfügbar (siehe BepInEx-Log)", "Upscaling non disponibile (vedi log di BepInEx)", "Skalowanie niedostępne (zobacz log BepInEx)", "超分辨率不可用（请查看 BepInEx 日志）" },

            ["Version_Off"] = new[] { "Off", "Desactivado", "Desativado", "Désactivé", "Aus", "Disattivato", "Wyłączone", "关闭" },
            ["Mode_NativeAA"] = new[] { "Native AA", "AA nativo", "AA nativo", "AA natif", "Natives AA", "AA nativo", "Natywny AA", "原生抗锯齿" },
            ["Mode_Quality"] = new[] { "Quality", "Calidad", "Qualidade", "Qualité", "Qualität", "Qualità", "Jakość", "质量" },
            ["Mode_Balanced"] = new[] { "Balanced", "Equilibrado", "Equilibrado", "Équilibré", "Ausgewogen", "Bilanciato", "Zrównoważony", "平衡" },
            ["Mode_Performance"] = new[] { "Performance", "Rendimiento", "Desempenho", "Performance", "Leistung", "Prestazioni", "Wydajność", "性能" },
            ["Mode_UltraPerformance"] = new[] { "Ultra Performance", "Ultra rendimiento", "Ultra desempenho", "Ultra performance", "Ultra-Leistung", "Ultra prestazioni", "Ultra wydajność", "超级性能" },

            ["NeedsDx12"] = new[]
            {
                "FSR, XeSS and DLSS need DirectX 12. In Steam, open Paralives > Properties, add -force-d3d12 to Launch Options and restart the game.",
                "FSR, XeSS y DLSS necesitan DirectX 12. En Steam, abre Paralives > Propiedades, agrega -force-d3d12 en Opciones de lanzamiento y reinicia el juego.",
                "O FSR, o XeSS e o DLSS precisam do DirectX 12. Na Steam, abra Paralives > Propriedades, adicione -force-d3d12 às Opções de inicialização e reinicie o jogo.",
                "FSR, XeSS et DLSS nécessitent DirectX 12. Dans Steam, ouvrez Paralives > Propriétés, ajoutez -force-d3d12 aux options de lancement et redémarrez le jeu.",
                "FSR, XeSS und DLSS benötigen DirectX 12. Öffne in Steam Paralives > Eigenschaften, füge -force-d3d12 zu den Startoptionen hinzu und starte das Spiel neu.",
                "FSR, XeSS e DLSS richiedono DirectX 12. Su Steam apri Paralives > Proprietà, aggiungi -force-d3d12 alle opzioni di avvio e riavvia il gioco.",
                "FSR, XeSS i DLSS wymagają DirectX 12. W Steam otwórz Paralives > Właściwości, dodaj -force-d3d12 do opcji uruchamiania i uruchom grę ponownie.",
                "FSR、XeSS 和 DLSS 需要 DirectX 12。请在 Steam 中打开 Paralives > 属性，在启动选项中添加 -force-d3d12，然后重启游戏。"
            },

            ["Tip_Upscaler"] = new[]
            {
                "Native AA keeps full resolution and only improves anti-aliasing. Quality to Ultra Performance render at a lower resolution (1.5x to 3x) and upscale: more FPS only when your graphics card is the bottleneck.",
                "AA nativo mantiene la resolución completa y solo mejora el antialiasing. De Calidad a Ultra rendimiento se renderiza a menor resolución (1,5x a 3x) y se escala: da más FPS solo si la tarjeta gráfica es el cuello de botella.",
                "AA nativo mantém a resolução total e só melhora o antisserrilhamento. De Qualidade a Ultra desempenho renderiza em resolução menor (1,5x a 3x) e amplia: mais FPS só quando a placa de vídeo é o gargalo.",
                "L'AA natif garde la pleine résolution et améliore seulement l'anticrénelage. De Qualité à Ultra performance, le rendu se fait en résolution réduite (1,5x à 3x) puis est agrandi : plus d'IPS seulement si la carte graphique est le facteur limitant.",
                "Natives AA behält die volle Auflösung und verbessert nur die Kantenglättung. Qualität bis Ultra-Leistung rendern in niedrigerer Auflösung (1,5x bis 3x) und skalieren hoch: mehr FPS nur, wenn die Grafikkarte der Engpass ist.",
                "L'AA nativo mantiene la risoluzione piena e migliora solo l'antialiasing. Da Qualità a Ultra prestazioni il rendering avviene a risoluzione ridotta (da 1,5x a 3x) e viene scalato: più FPS solo se la scheda video è il collo di bottiglia.",
                "Natywny AA zachowuje pełną rozdzielczość i poprawia tylko wygładzanie krawędzi. Od Jakości do Ultra wydajności obraz jest renderowany w niższej rozdzielczości (1,5x do 3x) i skalowany: więcej FPS tylko wtedy, gdy ogranicza karta graficzna.",
                "原生抗锯齿保持全分辨率，只提升抗锯齿效果。从质量到超级性能会以较低分辨率（1.5 至 3 倍）渲染再放大：只有在显卡是瓶颈时才会提高帧数。"
            },
            ["Tip_UpscalerVersion"] = new[]
            {
                "Off disables upscaling. FSR 4 runs on Radeon RX 9000 (and RX 7000 with recent drivers) and falls back to FSR 3 on other cards. Intel XeSS looks best on Intel Arc and also works on most other cards. NVIDIA DLSS needs an NVIDIA RTX card. If XeSS or DLSS cannot start, FSR is used.",
                "Desactivado apaga el escalado. FSR 4 funciona en Radeon RX 9000 (y RX 7000 con drivers recientes) y en otras tarjetas vuelve a FSR 3. Intel XeSS se ve mejor en Intel Arc y también funciona en la mayoría de las demás tarjetas. NVIDIA DLSS necesita una tarjeta NVIDIA RTX. Si XeSS o DLSS no pueden iniciarse, se usa FSR.",
                "Desativado desliga o upscaling. O FSR 4 funciona nas Radeon RX 9000 (e RX 7000 com drivers recentes) e volta para o FSR 3 nas outras placas. O Intel XeSS fica melhor nas Intel Arc e também funciona na maioria das outras placas. O NVIDIA DLSS precisa de uma placa NVIDIA RTX. Se o XeSS ou o DLSS não puderem iniciar, o FSR é usado.",
                "Désactivé coupe la mise à l'échelle. FSR 4 fonctionne sur Radeon RX 9000 (et RX 7000 avec un pilote récent) et revient à FSR 3 sur les autres cartes. Intel XeSS donne le meilleur résultat sur Intel Arc et fonctionne aussi sur la plupart des autres cartes. NVIDIA DLSS nécessite une carte NVIDIA RTX. Si XeSS ou DLSS ne peut pas démarrer, FSR est utilisé.",
                "Aus schaltet die Hochskalierung ab. FSR 4 läuft auf Radeon RX 9000 (und RX 7000 mit aktuellen Treibern) und fällt auf anderen Karten auf FSR 3 zurück. Intel XeSS sieht auf Intel Arc am besten aus und läuft auch auf den meisten anderen Karten. NVIDIA DLSS benötigt eine NVIDIA-RTX-Karte. Startet XeSS oder DLSS nicht, wird FSR verwendet.",
                "Disattivato spegne l'upscaling. FSR 4 funziona su Radeon RX 9000 (e RX 7000 con driver recenti) e sulle altre schede ripiega su FSR 3. Intel XeSS rende al meglio su Intel Arc e funziona anche sulla maggior parte delle altre schede. NVIDIA DLSS richiede una scheda NVIDIA RTX. Se XeSS o DLSS non si avviano, viene usato FSR.",
                "Wyłączone wyłącza skalowanie. FSR 4 działa na Radeon RX 9000 (oraz RX 7000 z nowymi sterownikami), a na innych kartach przełącza się na FSR 3. Intel XeSS działa najlepiej na Intel Arc, ale też na większości innych kart. NVIDIA DLSS wymaga karty NVIDIA RTX. Jeśli XeSS lub DLSS nie uruchomi się, używany jest FSR.",
                "关闭会停用超分辨率。FSR 4 可在 Radeon RX 9000（以及使用新驱动的 RX 7000）上运行，在其他显卡上会改用 FSR 3。Intel XeSS 在 Intel Arc 上效果最佳，也能在大多数其他显卡上运行。NVIDIA DLSS 需要 NVIDIA RTX 显卡。若 XeSS 或 DLSS 无法启动，则改用 FSR。"
            },
            ["Dlss_RequiresRtx"] = new[] { "requires NVIDIA RTX", "requiere NVIDIA RTX", "requer NVIDIA RTX", "nécessite NVIDIA RTX", "erfordert NVIDIA RTX", "richiede NVIDIA RTX", "wymaga NVIDIA RTX", "需要 NVIDIA RTX" },
            ["Tip_UpscalerSharpness"] = new[]
            {
                "Extra sharpening after upscaling. 0 = off. FSR and XeSS/DLSS keep separate values; XeSS and DLSS start at 0 because they already look sharp.",
                "Nitidez extra después del escalado. 0 = desactivada. FSR y XeSS/DLSS guardan valores separados; XeSS y DLSS empiezan en 0 porque ya se ven nítidos.",
                "Nitidez extra depois do upscaling. 0 = desativada. FSR e XeSS/DLSS guardam valores separados; XeSS e DLSS começam em 0 porque já ficam nítidos.",
                "Accentuation supplémentaire après la mise à l'échelle. 0 = désactivée. FSR et XeSS/DLSS gardent des valeurs séparées ; XeSS et DLSS démarrent à 0 car ils sont déjà nets.",
                "Zusätzliche Schärfung nach dem Upscaling. 0 = aus. FSR und XeSS/DLSS haben getrennte Werte; XeSS und DLSS starten bei 0, da sie schon scharf aussehen.",
                "Nitidezza aggiuntiva dopo l'upscaling. 0 = disattivata. FSR e XeSS/DLSS hanno valori separati; XeSS e DLSS partono da 0 perché sono già nitidi.",
                "Dodatkowe wyostrzanie po skalowaniu. 0 = wyłączone. FSR i XeSS/DLSS mają osobne wartości; XeSS i DLSS zaczynają od 0, bo już są ostre.",
                "超分辨率之后的额外锐化。0 = 关闭。FSR 与 XeSS/DLSS 分别保存数值；XeSS 和 DLSS 默认是 0，因为它们本身已足够清晰。"
            },
            ["Tip_NeedsDx12"] = new[]
            {
                "This mod only runs FSR, XeSS and DLSS on DirectX 12. Add -force-d3d12 to the game's launch options in Steam.",
                "Este mod solo ejecuta FSR, XeSS y DLSS con DirectX 12. Agrega -force-d3d12 a las opciones de lanzamiento del juego en Steam.",
                "Este mod só executa FSR, XeSS e DLSS com DirectX 12. Adicione -force-d3d12 às opções de inicialização do jogo na Steam.",
                "Ce mod n'exécute FSR, XeSS et DLSS qu'avec DirectX 12. Ajoutez -force-d3d12 aux options de lancement du jeu dans Steam.",
                "Diese Mod nutzt FSR, XeSS und DLSS nur unter DirectX 12. Füge -force-d3d12 zu den Startoptionen des Spiels in Steam hinzu.",
                "Questa mod esegue FSR, XeSS e DLSS solo con DirectX 12. Aggiungi -force-d3d12 alle opzioni di avvio del gioco su Steam.",
                "Ten mod uruchamia FSR, XeSS i DLSS tylko w DirectX 12. Dodaj -force-d3d12 do opcji uruchamiania gry w Steam.",
                "此模组仅在 DirectX 12 下运行 FSR、XeSS 和 DLSS。请在 Steam 的游戏启动选项中添加 -force-d3d12。"
            },
        };

        /// <summary>Text for a key containing "ParalivesUpscaling_" (the game may add its own prefix in front), or null.</summary>
        internal static string Translate(string key)
        {
            int at = key.IndexOf(Prefix, StringComparison.Ordinal);
            if (at < 0) return null;
            string sub = key.Substring(at + Prefix.Length);

            // technology entries carry the exact version the GPU/driver offers
            string offered = sub == "Version_FSR3" ? Native.Fsr3Name : sub == "Version_FSR4" ? Native.Fsr4Name
                           : sub == "Version_XeSS" ? Native.XessName : sub == "Version_DLSS" ? Native.DlssName : null;
            if (offered != null)
            {
                if (sub == "Version_DLSS")  // greyed out without an RTX card (MenuIntegration ignores picking it)
                    return offered.Length > 0 ? "NVIDIA DLSS " + offered : "<color=#8C8C8C>NVIDIA DLSS (" + Pick(Table["Dlss_RequiresRtx"]) + ")</color>";
                if (sub == "Version_XeSS") return offered.Length > 0 ? "Intel XeSS " + offered : "Intel XeSS";
                return offered.Length > 0 ? "FSR " + offered : (sub == "Version_FSR3" ? "FSR 3" : "FSR 4");
            }

            if (sub == "FrameGeneration")  // greyed out while it cannot run (MenuIntegration ignores switching it on without its swapchain)
            {
                string label = Pick(Table[sub]);
                if (Native.FrameGenState == 0) return "<color=#8C8C8C>" + label + " (" + Pick(Table["FrameGen_Unavailable"]) + ")</color>";
                if (Plugin.O.Version.Value == FsrVersion.Off) return "<color=#8C8C8C>" + label + " (" + Pick(Table["FrameGen_NeedsUpscaling"]) + ")</color>";
                return label;
            }
            // frame generation entries: the one not matching this session's swapchain asks for a restart
            if (sub == "FrameGen_Off") return Pick(Table["Version_Off"]);
            if (sub == "FrameGen_FSR") return "AMD FSR";
            if (sub == "FrameGen_XeSS") return "Intel XeSS";

            string[] texts;
            return Table.TryGetValue(sub, out texts) ? Pick(texts) : null;
        }

        private static string Pick(string[] texts)
        {
            string locale = "en";
            try { locale = (TranslationManager.GetCurrentLocale() ?? "en").ToLowerInvariant(); } catch { }
            for (int i = 0; i < Locales.Length && i < texts.Length; i++)
                if (locale.StartsWith(Locales[i], StringComparison.Ordinal)) return texts[i];
            return texts[0];
        }
    }

    [HarmonyPatch(typeof(TranslationManager), nameof(TranslationManager.Get), typeof(string), typeof(string[]))]
    internal static class TranslationPatch
    {
        private static bool Prefix(string key, ref string __result)
        {
            if (key == null || key.IndexOf("ParalivesUpscaling_", StringComparison.Ordinal) < 0) return true;
            string text = Localization.Translate(key);
            if (text == null) return true;
            __result = text;
            return false;
        }
    }
}
