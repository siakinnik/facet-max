// Russian translation.
#include "i18n/i18n.h"

namespace max_module {

namespace {
const facet::i18n::Table& ru() {
    static const facet::i18n::Table table = {
        {"MAX keeps closing. Start it again or reinstall it.",
         "MAX постоянно закрывается. Запустите его снова или переустановите."},
        {"Downloading {}%", "Загрузка {}%"},
        {"Installing…", "Установка…"},
        {"Not installed", "Не установлен"},
        {"{} new", "новых: {}"},
        {"MAX {} is available", "Доступен MAX {}"},
        {"Download {} MB and restart MAX.", "Скачать {} МБ и перезапустить MAX."},
        {"Update", "Обновить"},
        {"MAX messenger", "Мессенджер MAX"},
        {"Downloading", "Загрузка"},
        {"Checking the download", "Проверка загрузки"},
        {"Unpacking", "Распаковка"},
        {"{} of {} MB", "{} из {} МБ"},
        {"Cancel", "Отмена"},
        {"MAX is not installed yet. It is downloaded from download.max.ru, the official site, and runs only inside "
         "this module.",
         "MAX ещё не установлен. Он скачивается с официального сайта download.max.ru и работает только внутри "
         "этого модуля."},
        {"Download", "Загрузка"},
        {"{} MB, about {} MB on disk", "{} МБ, на диске около {} МБ"},
        {"Install MAX", "Установить MAX"},
        {"starting…", "запускается…"},
        {"running", "работает"},
        {"waiting for Wayland…", "ждёт Wayland…"},
        {"stopped", "остановлен"},
        {"State", "Состояние"},
        {"Version", "Версия"},
        {"Available", "Доступна"},
        {"Start MAX", "Запустить MAX"},
        {"Stop MAX", "Остановить MAX"},
        {"Error", "Ошибка"},
        {"Data", "Данные"},
        {"Sign out and delete messages on this panel", "Выйти и удалить сообщения с этой панели"},
        {"Remove the app (the login stays)", "Удалить приложение (вход сохранится)"},
    };
    return table;
}
}  // namespace

void register_translations(facet::i18n::Catalog& catalog) { catalog.add("ru", ru()); }

}  // namespace max_module
