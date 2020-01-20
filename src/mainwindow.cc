/*
 * Copyright (C) 2019-2020 Ashar Khan <ashar786khan@gmail.com>
 *
 * This file is part of CPEditor.
 *
 * CPEditor is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * I will not be responsible if CPEditor behaves in unexpected way and
 * causes your ratings to go down and or loose any important contest.
 *
 * Believe Software is "Software" and it isn't immune to bugs.
 *
 */

#include "mainwindow.hpp"

#include <Compiler.hpp>
#include <DiffViewer.hpp>
#include <EditorTheme.hpp>
#include <MessageLogger.hpp>
#include <QCXXHighlighter>
#include <QFileDialog>
#include <QFont>
#include <QFontDialog>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPythonCompleter>
#include <QPythonHighlighter>
#include <QShortcut>
#include <QSyntaxStyle>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <Runner.hpp>
#include <expand.hpp>

#include "../ui/ui_mainwindow.h"

// ***************************** RAII  ****************************
MainWindow::MainWindow(QString fileOpen, const Settings::SettingsData &data, int index, QWidget *parent)
    : QMainWindow(parent), untitledIndex(index), ui(new Ui::MainWindow), fileWatcher(new QFileSystemWatcher(this))
{
    ui->setupUi(this);
    setEditor();
    setupCore();
    connect(fileWatcher, SIGNAL(fileChanged(const QString &)), this, SLOT(onFileWatcherChanged(const QString &)));
    setSettingsData(data, true);
    loadFile(fileOpen);
}

MainWindow::~MainWindow()
{
    killProcesses();

    if (cftools != nullptr)
        delete cftools;
    if (tmpDir != nullptr)
        delete tmpDir;

    delete ui;
    delete editor;
    delete formatter;
    delete fileWatcher;
}

// ************************* RAII HELPER *****************************

void MainWindow::setEditor()
{
    editor = new QCodeEditor();
    editor->setMinimumWidth(600);
    editor->setMinimumHeight(300);
    editor->setAcceptDrops(false);

    ui->verticalLayout_8->addWidget(editor);

    input[0] = ui->in1;
    input[1] = ui->in2;
    input[2] = ui->in3;

    output[0] = ui->out1;
    output[1] = ui->out2;
    output[2] = ui->out3;

    verdict[0] = ui->out1_verdict;
    verdict[1] = ui->out2_verdict;
    verdict[2] = ui->out3_verdict;

    for (int i = 0; i < 3; ++i)
    {
        input[i]->setWordWrapMode(QTextOption::NoWrap);
        output[i]->setWordWrapMode(QTextOption::NoWrap);
        input[i]->setAcceptDrops(false);
        expected[i] = new QString;
    }

    QObject::connect(editor, SIGNAL(textChanged()), this, SLOT(onTextChangedTriggered()));

    for (auto i : {0, 1, 2})
        updateVerdict(UNKNOWN, i);
}

void MainWindow::setupCore()
{
    using namespace Core;
    formatter = new Formatter(data.clangFormatBinary, data.clangFormatStyle, &log);
    log.setContainer(ui->compiler_edit);
}

void MainWindow::compile()
{
    killProcesses();
    compiler = new Core::Compiler();
    if (saveTemp("Compiler"))
    {
        QString command;
        if (language == "Cpp")
            command = data.compileCommandCpp;
        else if (language == "Java")
            command = data.compileCommandJava;
        else
            return;
        connect(compiler, SIGNAL(compilationStarted()), this, SLOT(onCompilationStarted()));
        connect(compiler, SIGNAL(compilationFinished(const QString &)), this,
                SLOT(onCompilationFinished(const QString &)));
        connect(compiler, SIGNAL(compilationErrorOccured(const QString &)), this,
                SLOT(onCompilationErrorOccured(const QString &)));
        compiler->start(tmpPath(), command, language);
    }
}

void MainWindow::run()
{
    killProcesses();
    clearTests(true);

    QString command, args;
    if (language == "Cpp")
    {
        args = data.runtimeArgumentsCpp;
    }
    else if (language == "Java")
    {
        command = data.runCommandJava;
        args = data.runtimeArgumentsJava;
    }
    else if (language == "Python")
    {
        command = data.runCommandPython;
        args = data.runtimeArgumentsPython;
    }
    else
    {
        log.warn("Runner", "Wrong language, please set the language");
        return;
    }

    bool isRun = false;

    for (int i = 0; i < 3; ++i)
    {
        if (!input[i]->toPlainText().trimmed().isEmpty())
        {
            isRun = true;
            runner[i] = new Core::Runner(i);
            connect(runner[i], SIGNAL(runStarted(int)), this, SLOT(onRunStarted(int)));
            connect(runner[i], SIGNAL(runFinished(int, const QString &, const QString &, int, int)), this,
                    SLOT(onRunFinished(int, const QString &, const QString &, int, int)));
            connect(runner[i], SIGNAL(runErrorOccured(int, const QString &)), this,
                    SLOT(onRunErrorOccured(int, const QString &)));
            connect(runner[i], SIGNAL(runTimeout(int)), this, SLOT(onRunTimeout(int)));
            connect(runner[i], SIGNAL(runKilled(int)), this, SLOT(onRunKilled(int)));
            runner[i]->run(tmpPath(), language, command, args, input[i]->toPlainText(), data.timeLimit);
        }
    }

    if (!isRun)
        log.warn("Runner", "All inputs are empty, nothing to run");
}

void MainWindow::clearTests(bool outputOnly)
{
    for (int i = 0; i < 3; ++i)
    {
        if (!outputOnly)
        {
            input[i]->clear();
            expected[i]->clear();
        }
        output[i]->clear();
        updateVerdict(UNKNOWN, i);
    }
}

void MainWindow::loadTests()
{
    if (isUntitled() || !data.shouldSaveTests)
        return;

    QFileInfo fileInfo(filePath);
    QString testFile = fileInfo.canonicalPath() + "/" + fileInfo.completeBaseName();

    for (int i = 0; i < 3; ++i)
    {
        if (QFile::exists(testFile + QString::number(i + 1) + ".in"))
        {
            QFile inputFile(testFile + QString::number(i + 1) + ".in");
            inputFile.open(QIODevice::ReadOnly | QFile::Text);
            if (inputFile.isOpen())
            {
                auto cursor = input[i]->textCursor();
                cursor.select(QTextCursor::Document);
                cursor.insertText(inputFile.readAll());
            }
            else
            {
                log.error("Tests",
                          "Failed to open" + inputFile.fileName().toStdString() + ". Do I have read permissions?");
            }
        }

        if (QFile::exists(testFile + QString::number(i + 1) + ".ans"))
        {
            QFile answerFile(testFile + QString::number(i + 1) + ".ans");
            answerFile.open(QIODevice::ReadOnly | QFile::Text);
            if (answerFile.isOpen())
            {
                *expected[i] = (answerFile.readAll());
            }
            else
            {
                log.error("Tests",
                          "Failed to open" + answerFile.fileName().toStdString() + ". Do I have read permissions?");
            }
        }
    }
}

void MainWindow::saveTests()
{
    if (isUntitled() || !data.shouldSaveTests)
        return;

    QFileInfo fileInfo(filePath);
    QString testFile = fileInfo.canonicalPath() + "/" + fileInfo.completeBaseName();

    for (int i = 0; i < 3; ++i)
    {
        if (!input[i]->toPlainText().trimmed().isEmpty())
        {
            QFile inputFile(testFile + QString::number(i + 1) + ".in");
            inputFile.open(QIODevice::WriteOnly | QFile::Text);
            if (!inputFile.isOpen() || inputFile.write(input[i]->toPlainText().toStdString().c_str()) == -1)
            {
                log.error("Tests", "Failed to save Input #" + std::to_string(i + 1) + ". Do I have write permission?");
            }
        }

        if (!expected[i]->trimmed().isEmpty())
        {
            QFile answerFile(testFile + QString::number(i + 1) + ".ans");
            answerFile.open(QIODevice::WriteOnly | QFile::Text);
            if (!answerFile.isOpen() || answerFile.write(expected[i]->toStdString().c_str()) == -1)
            {
                log.error("Tests",
                          "Failed to save Expected #" + std::to_string(i + 1) + ". Do I have write permission?");
            }
        }
    }
}

void MainWindow::setCFToolsUI()
{
    if (submitToCodeforces == nullptr)
    {
        submitToCodeforces = new QPushButton("Submit Solution", this);
        cftools = new Network::CFTools(&log);
        ui->horizontalLayout_9->addWidget(submitToCodeforces);
        connect(submitToCodeforces, &QPushButton::clicked, this, [this] {
            auto response = QMessageBox::warning(
                this, "Sure to submit",
                "Are you sure you want to submit this solution to Codeforces?\n\n URL: " + problemURL +
                    "\n Language : " + language,
                QMessageBox::Yes | QMessageBox::No);

            if (response == QMessageBox::Yes)
            {
                if (saveTemp("CF Tool Saver"))
                    cftools->submit(tmpPath(), problemURL, language);
            }
        });
    }
    if (!Network::CFTools::check())
    {
        submitToCodeforces->setEnabled(false);
        log.error("CFTools", "You will not be able to submit code to Codeforces because CFTools is not installed or is "
                             "not on SYSTEM PATH");
    }
}

QString MainWindow::getFileName() const
{
    return isUntitled() ? "Untitled-" + QString::number(untitledIndex) : QFileInfo(filePath).fileName();
}

QString MainWindow::getFilePath() const
{
    return filePath;
}

QString MainWindow::getProblemURL() const
{
    return problemURL;
}

QString MainWindow::getTabTitle(bool complete)
{
    auto tabTitle = complete && !isUntitled() ? filePath : getFileName();
    if (isTextChanged())
        tabTitle += " *";
    return tabTitle;
}

bool MainWindow::isUntitled() const
{
    return filePath.isEmpty();
}

void MainWindow::updateVerdict(Verdict _verdict, int id)
{
    QString verdict_text, style_sheet;

    switch (_verdict)
    {
    case ACCEPTED:
        verdict_text = "Verdict : AC";
        style_sheet = "QLabel { color : rgb(0, 180, 0); }";
        break;
    case WRONG_ANSWER:
        verdict_text = "Verdict : WA";
        style_sheet = "QLabel { color : rgb(255, 0, 0); }";
        break;
    case UNKNOWN:
        verdict_text = "Verdict : **";
        style_sheet = "";
        break;
    }

    verdict[id]->setText(verdict_text);
    verdict[id]->setStyleSheet(style_sheet);
}

void MainWindow::applyCompanion(Network::CompanionData data)
{
    if (isUntitled() && !isTextChanged())
    {
        QString meta = data.toMetaString();
        meta.prepend("\n");
        meta.append("Powered by CP Editor (https://github.com/coder3101/cp-editor)");

        if (language == "Python")
            meta.replace('\n', "\n# ");
        else
            meta.replace('\n', "\n// ");

        editor->setPlainText(meta + "\n\n" + editor->toPlainText());
    }

    clearTests();

    if (data.testcases.size() > 3)
    {
        log.warn("CP Editor", "More than 3 testcase were produced. Only First 3 will be used");
    }

    for (int i = 0; i < data.testcases.size() && i < 3; ++i)
    {
        input[i]->setPlainText(data.testcases[i].input);
        *expected[i] = (data.testcases[i].output);
    }
    problemURL = data.url;
    if (problemURL.contains("codeforces.com"))
        setCFToolsUI();
    onTextChangedTriggered();
}

void MainWindow::setSettingsData(const Settings::SettingsData &data, bool shouldPerformDigonistic)
{
    this->data = data;
    formatter->updateBinary(data.clangFormatBinary);
    formatter->updateStyle(data.clangFormatStyle);

    editor->setTabReplace(data.isTabsReplaced);
    editor->setTabReplaceSize(data.tabStop);
    editor->setAutoIndentation(data.isAutoIndent);
    editor->setAutoParentheses(data.isAutoParenthesis);

    if (!data.font.isEmpty())
    {
        QFont font;
        font.fromString(data.font);
        editor->setFont(font);
    }

    const int tabStop = data.tabStop;
    QFontMetrics metric(editor->font());
    editor->setTabReplaceSize(tabStop);
    editor->setTabStopDistance(tabStop * metric.horizontalAdvance("9"));

    if (data.isWrapText)
        editor->setWordWrapMode(QTextOption::WordWrap);
    else
        editor->setWordWrapMode(QTextOption::NoWrap);

    if (data.editorTheme == "Light")
        editor->setSyntaxStyle(Themes::EditorTheme::getLightTheme());
    else if (data.editorTheme == "Drakula")
        editor->setSyntaxStyle(Themes::EditorTheme::getDrakulaTheme());
    else if (data.editorTheme == "Monkai")
        editor->setSyntaxStyle(Themes::EditorTheme::getMonkaiTheme());
    else if (data.editorTheme == "Solarised")
        editor->setSyntaxStyle(Themes::EditorTheme::getSolarisedTheme());
    else if (data.editorTheme == "Solarised Dark")
        editor->setSyntaxStyle(Themes::EditorTheme::getSolarisedDarkTheme());
    else
    {
        log.warn("Themes", "Editor theme is set to invalid value. Fallback to Light");
        editor->setSyntaxStyle(Themes::EditorTheme::getLightTheme());
    }

    if (!isLanguageSet)
    {
        setLanguage(data.defaultLanguage);
    }
    if (shouldPerformDigonistic)
    {
        performCoreDiagonistics();
    }
}

void MainWindow::save(bool force)
{
    saveFile(force ? SaveUntitled : IgnoreUntitled, "Save");
}

void MainWindow::saveAs()
{
    saveFile(SaveAs, "Save as");
}

void MainWindow::onTextChangedTriggered()
{
    emit editorChanged(this);
}

void MainWindow::on_compile_clicked()
{
    compileOnly();
}

void MainWindow::on_runOnly_clicked()
{
    runOnly();
}

void MainWindow::on_run_clicked()
{
    compileAndRun();
}

void MainWindow::compileOnly()
{
    afterCompile = Nothing;
    log.clear();
    compile();
}

void MainWindow::runOnly()
{
    log.clear();
    run();
}

void MainWindow::compileAndRun()
{
    afterCompile = Run;
    log.clear();
    compile();
}

void MainWindow::formatSource()
{
    formatter->format(editor, filePath, language, true);
}

void MainWindow::setLanguage(QString lang)
{
    log.clear();
    language = lang;
    if (lang == "Python")
    {
        editor->setHighlighter(new QPythonHighlighter);
        editor->setCompleter(new QPythonCompleter);
        ui->changeLanguageButton->setText("Python");
    }
    else if (lang == "Java")
    {
        editor->setHighlighter(new QCXXHighlighter);
        editor->setCompleter(nullptr);
        ui->changeLanguageButton->setText("Java");
    }
    else
    {
        language = "Cpp";
        if (lang != "Cpp")
            log.warn("CP Editor", "Unknown lanague set, fallback to Cpp");
        editor->setHighlighter(new QCXXHighlighter);
        editor->setCompleter(nullptr);
        ui->changeLanguageButton->setText("Cpp");
    }
    performCoreDiagonistics();
    isLanguageSet = true;
}

QString MainWindow::getLanguage()
{
    return language;
}

MessageLogger *MainWindow::getLogger()
{
    return &log;
}

void MainWindow::insertText(QString text)
{
    editor->insertPlainText(text);
}

void MainWindow::focusOnEditor()
{
    editor->setFocus();
}

void MainWindow::detachedExecution()
{
    afterCompile = RunDetached;
    log.clear();
    compile();
}

void MainWindow::killProcesses()
{
    if (compiler != nullptr)
    {
        delete compiler;
        compiler = nullptr;
    }

    for (auto &t : runner)
    {
        if (t != nullptr)
        {
            delete t;
            t = nullptr;
        }
    }

    if (detachedRunner != nullptr)
    {
        delete detachedRunner;
        detachedRunner = nullptr;
    }
}

// ****************************** Context Menus **************************/

void MainWindow::on_in1_customContextMenuRequested(const QPoint &pos)
{
    QMenu *stdMenu = ui->in1->createStandardContextMenu(pos);
    QAction *newAction = new QAction("Expand");

    QObject::connect(newAction, &QAction::triggered, this, [this] {
        auto ptr = new Expand(ui->in1, &this->log, this);
        ptr->setTitle("Input 1");
        ptr->setUpdate(true);
        ptr->setReadFile(true);
        ptr->show();
    });

    stdMenu->insertAction(stdMenu->actions().first(), newAction);
    stdMenu->popup(ui->in1->viewport()->mapToGlobal(pos));
}

void MainWindow::on_in2_customContextMenuRequested(const QPoint &pos)
{
    QMenu *stdMenu = ui->in2->createStandardContextMenu(pos);
    QAction *newAction = new QAction("Expand");

    QObject::connect(newAction, &QAction::triggered, this, [this] {
        auto ptr = new Expand(ui->in2, &this->log, this);
        ptr->setTitle("Input 2");
        ptr->setUpdate(true);
        ptr->setReadFile(true);
        ptr->show();
    });

    stdMenu->insertAction(stdMenu->actions().first(), newAction);
    stdMenu->popup(ui->in2->viewport()->mapToGlobal(pos));
}

void MainWindow::on_in3_customContextMenuRequested(const QPoint &pos)
{
    QMenu *stdMenu = ui->in3->createStandardContextMenu(pos);
    QAction *newAction = new QAction("Expand");

    QObject::connect(newAction, &QAction::triggered, this, [this] {
        auto ptr = new Expand(ui->in3, &this->log, this);
        ptr->setTitle("Input 3");
        ptr->setUpdate(true);
        ptr->setReadFile(true);
        ptr->show();
    });

    stdMenu->insertAction(stdMenu->actions().first(), newAction);
    stdMenu->popup(ui->in3->viewport()->mapToGlobal(pos));
}

void MainWindow::on_compiler_edit_customContextMenuRequested(const QPoint &pos)
{
    QMenu *stdMenu = ui->compiler_edit->createStandardContextMenu(pos);
    QAction *newAction = new QAction("Expand");

    QObject::connect(newAction, &QAction::triggered, this, [this] {
        auto ptr = new Expand(this->ui->compiler_edit, &this->log, this);
        ptr->show();
    });

    stdMenu->insertAction(stdMenu->actions().first(), newAction);
    stdMenu->popup(ui->compiler_edit->viewport()->mapToGlobal(pos));
}

void MainWindow::on_out1_customContextMenuRequested(const QPoint &pos)
{
    QMenu *stdMenu = ui->out1->createStandardContextMenu(pos);
    QAction *newAction = new QAction("Expand");

    QObject::connect(newAction, &QAction::triggered, this, [this] {
        auto ptr = new Expand(ui->out1, &this->log, this);
        ptr->setTitle("Output 1");
        ptr->setUpdate(false);
        ptr->setReadFile(false);
        ptr->show();
    });

    stdMenu->insertAction(stdMenu->actions().first(), newAction);
    stdMenu->popup(ui->out1->viewport()->mapToGlobal(pos));
}
void MainWindow::on_out2_customContextMenuRequested(const QPoint &pos)
{
    QMenu *stdMenu = ui->out2->createStandardContextMenu(pos);
    QAction *newAction = new QAction("Expand");

    QObject::connect(newAction, &QAction::triggered, this, [this] {
        auto ptr = new Expand(ui->out2, &this->log, this);
        ptr->setTitle("Output 2");
        ptr->setUpdate(false);
        ptr->setReadFile(false);
        ptr->show();
    });

    stdMenu->insertAction(stdMenu->actions().first(), newAction);
    stdMenu->popup(ui->out2->viewport()->mapToGlobal(pos));
}

void MainWindow::on_out3_customContextMenuRequested(const QPoint &pos)
{
    QMenu *stdMenu = ui->out3->createStandardContextMenu(pos);
    QAction *newAction = new QAction("Expand");

    QObject::connect(newAction, &QAction::triggered, this, [this] {
        auto ptr = new Expand(ui->out3, &this->log, this);
        ptr->setTitle("Output 3");
        ptr->setUpdate(false);
        ptr->setReadFile(false);
        ptr->show();
    });

    stdMenu->insertAction(stdMenu->actions().first(), newAction);
    stdMenu->popup(ui->out3->viewport()->mapToGlobal(pos));
}

//********************* DIFF Showers ******************

void MainWindow::on_out1_diff_clicked()
{
    auto ptr = new DiffViewer(expected[0], ui->out1, &log);
    ptr->setTitle("Diffviewer for Case #1");
    ptr->show();
}

void MainWindow::on_out2_diff_clicked()
{
    auto ptr = new DiffViewer(expected[1], ui->out2, &log);
    ptr->setTitle("Diffviewer for Case #2");
    ptr->show();
}

void MainWindow::on_out3_diff_clicked()
{
    auto ptr = new DiffViewer(expected[2], ui->out3, &log);
    ptr->setTitle("Diffviewer for Case #3");
    ptr->show();
}

//***************** HELPER FUNCTIONS *****************

bool MainWindow::isVerdictPass(QString output, QString expected)
{
    output = output.remove('\r');
    expected = expected.remove('\r');
    auto a_lines = output.split('\n');
    auto b_lines = expected.split('\n');
    for (int i = 0; i < a_lines.size() || i < b_lines.size(); ++i)
    {
        if (i >= a_lines.size())
        {
            if (b_lines[i].trimmed().isEmpty())
                continue;
            else
                return false;
        }
        if (i >= b_lines.size())
        {
            if (a_lines[i].trimmed().isEmpty())
                continue;
            else
                return false;
        }
        auto a_words = a_lines[i].split(' ');
        auto b_words = b_lines[i].split(' ');
        for (int j = 0; j < a_words.size() || j < b_words.size(); ++j)
        {
            if (j >= a_words.size())
            {
                if (b_words[j].trimmed().isEmpty())
                    continue;
                else
                    return false;
            }
            if (j >= b_words.size())
            {
                if (a_words[j].trimmed().isEmpty())
                    continue;
                else
                    return false;
            }
            if (a_words[j] != b_words[j])
                return false;
        }
    }
    return true;
}

void MainWindow::setText(const QString &text, bool saveCursor)
{
    if (saveCursor)
    {
        auto cursor = editor->textCursor();
        int old_pos = cursor.position();
        cursor.select(QTextCursor::Document);
        cursor.insertText(text);
        cursor.setPosition(old_pos);
        editor->setTextCursor(cursor);
    }
    else
    {
        auto cursor = editor->textCursor();
        cursor.select(QTextCursor::Document);
        cursor.insertText(text);
    }
}

void MainWindow::updateWatcher()
{
    onTextChangedTriggered();
    if (!fileWatcher->files().isEmpty())
        fileWatcher->removePaths(fileWatcher->files());
    if (!isUntitled())
        fileWatcher->addPath(filePath);
}

void MainWindow::loadFile(QString path)
{
    bool samePath = filePath == path;
    filePath = path;
    updateWatcher();
    if (!samePath)
        emit editorChanged(this);

    if (!QFile::exists(path))
    {
        if (isUntitled())
        {
            QString templatePath;

            if (language == "Cpp")
                templatePath = data.templateCpp;
            else if (language == "Java")
                templatePath = data.templateJava;
            else if (language == "Python")
                templatePath = data.templatePython;

            QFile f(templatePath);
            f.open(QIODevice::ReadOnly | QIODevice::Text);

            if (f.isOpen())
                path = templatePath;
            else
                return;
        }
        else
        {
            return;
        }
    }

    QFile openFile(path);
    openFile.open(QIODevice::ReadOnly | QFile::Text);
    if (openFile.isOpen())
    {
        savedText = openFile.readAll();
        setText(savedText, samePath);
    }
    else
    {
        log.warn("Loader", "Failed to load " + path.toStdString() + ". Do I have read permission?");
        return;
    }

    loadTests();
}

bool MainWindow::saveFile(SaveMode mode, std::string head)
{
    if (data.isFormatOnSave)
        formatter->format(editor, filePath, language, false);

    if (mode == SaveAs || (isUntitled() && mode == SaveUntitled))
    {
        emit confirmTriggered(this);
        auto newFilePath = QFileDialog::getSaveFileName(
            this, tr("Save File"), "", "Source Files (*.cpp *.hpp *.h *.cc *.cxx *.c *.py *.py3 *.java)");
        if (newFilePath.isEmpty())
            return false;

        QFile openFile(newFilePath);
        openFile.open(QIODevice::WriteOnly | QFile::Text);

        if (!openFile.isOpen() || openFile.write(editor->toPlainText().toStdString().c_str()) == -1)
        {
            log.error(head, "Failed to save file to [" + newFilePath.toStdString() + "]. Do I have write permission?");
            return false;
        }

        filePath = newFilePath;
        updateWatcher();
        emit editorChanged(this);

        auto suffix = QFileInfo(filePath).suffix();
        if (suffix == ".cpp" || suffix == ".hpp" || suffix == ".h" || suffix == ".cc" || suffix == ".cxx" ||
            suffix == ".c")
            setLanguage("Cpp");
        else if (suffix == "java")
            setLanguage("Java");
        else if (suffix == "py" || suffix == "py3")
            setLanguage("Python");
    }
    else if (!isUntitled())
    {
        QFile openFile(filePath);
        openFile.open(QFileDevice::WriteOnly | QFile::Text);
        if (!openFile.isOpen() || openFile.write(editor->toPlainText().toStdString().c_str()) == -1)
        {
            log.error(head, "Failed to save file to [" + filePath.toStdString() + "]. Do I have write permission?");
            return false;
        }
        updateWatcher();
    }
    else
    {
        return false;
    }

    saveTests();

    return true;
}

bool MainWindow::saveTemp(std::string head)
{
    if (!saveFile(IgnoreUntitled, head))
    {
        if (tmpDir != nullptr)
        {
            delete tmpDir;
        }

        tmpDir = new QTemporaryDir();
        if (!tmpDir->isValid())
        {
            log.error(head, "Failed to create temporary directory");
            return false;
        }

        QFile tmpFile(tmpPath());
        tmpFile.open(QIODevice::WriteOnly | QIODevice::Text);

        if (!tmpFile.isOpen() || tmpFile.write(editor->toPlainText().toStdString().c_str()) == -1)
        {
            log.error(head, "Failed to save to " + tmpFile.fileName().toStdString());
            return false;
        }
    }

    return true;
}

QString MainWindow::tmpPath()
{
    if (!isUntitled())
        return filePath;
    if (tmpDir == nullptr || !tmpDir->isValid())
    {
        if (!saveTemp("Temp Saver"))
        {
            return "";
        }
    }
    QString name;
    if (!isUntitled())
        name = getFileName();
    else if (language == "Cpp")
        name = "sol.cpp";
    else if (language == "Java")
        name = "sol.java";
    else if (language == "Python")
        name = "sol.py";
    else
    {
        log.error("Temp Saver", "Please set the language");
        return "";
    }
    return tmpDir->filePath(name);
}

bool MainWindow::isTextChanged()
{
    if (QFile::exists(filePath))
        filePath = QFileInfo(filePath).canonicalFilePath();

    QString templatePath;

    if (language == "Cpp")
        templatePath = data.templateCpp;
    else if (language == "Java")
        templatePath = data.templateJava;
    else if (language == "Python")
        templatePath = data.templatePython;
    else
        return false;

    if (isUntitled())
    {
        QFile f(templatePath);
        f.open(QIODevice::ReadOnly | QFile::Text);
        if (f.isOpen())
            return editor->toPlainText() != f.readAll();
        else
            return !editor->toPlainText().isEmpty();
    }
    else
    {
        QFile openFile(filePath);
        openFile.open(QIODevice::ReadOnly | QFile::Text);

        if (openFile.isOpen())
            return openFile.readAll() != editor->toPlainText();
        else
            return true;
    }
}

bool MainWindow::closeConfirm()
{
    bool confirmed = !isTextChanged();
    if (!confirmed)
    {
        emit confirmTriggered(this);
        auto res = QMessageBox::warning(
            this, "Save changes?",
            "Save changes to " + (isUntitled() ? QString("New File") : getFileName()) + " before closing?",
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (res == QMessageBox::Save)
            confirmed = saveFile(SaveUntitled, "Save");
        else if (res == QMessageBox::Discard)
            confirmed = true;
    }
    return confirmed;
}

void MainWindow::on_changeLanguageButton_clicked()
{
    bool ok = false;
    int curr = 0;

    if (language == "Cpp")
        curr = 0;
    else if (language == "Java")
        curr = 1;
    else if (language == "Python")
        curr = 2;

    auto response = QInputDialog::getItem(this, "Set Tab language", "Set the language to use in this Tab",
                                          {"Cpp", "Java", "Python"}, curr, false, &ok);

    if (ok)
        setLanguage(response);
}

void MainWindow::onFileWatcherChanged(const QString &path)
{
    onTextChangedTriggered();

    auto currentText = editor->toPlainText();

    QString fileText;
    QFile openFile(path);

    if (openFile.exists())
    {
        openFile.open(QIODevice::ReadOnly | QIODevice::Text);
        if (openFile.isOpen())
        {
            fileText = openFile.readAll();

            if (fileText == currentText)
            {
                savedText = fileText;
                return;
            }

            if (savedText == currentText)
            {
                loadFile(path);
                return;
            }

            auto reload = QMessageBox::question(
                this, "Reload?", "\"" + filePath + "\"\n\nhas been changed on disk.\nDo you want to reload it?");

            if (reload == QMessageBox::StandardButton::Yes)
            {
                loadFile(path);
                return;
            }
        }
    }
}

QSplitter *MainWindow::getSplitter()
{
    return ui->splitter;
}

void MainWindow::performCoreDiagonistics()
{
    log.clear();
    bool formatResult = Core::Formatter::check(data.clangFormatBinary, data.clangFormatStyle);
    bool compilerResult = true;
    bool runResults = true;

    if (language == "Cpp")
        compilerResult = Core::Compiler::check(data.compileCommandCpp);

    if (language == "Java")
    {
        compilerResult = Core::Compiler::check(data.compileCommandJava);
        runResults = Core::Compiler::check(data.runCommandJava);
    }

    if (language == "Python")
        compilerResult = Core::Compiler::check(data.runCommandPython);

    if (!formatResult)
        log.warn("Formatter", "Code formatting failed to work. Please check whether the clang-format binary is in the "
                              "PATH and the style is valid.");
    if (!compilerResult)
        log.error("Compiler", "Compiler command for " + language.toStdString() + " is invalid. Is compiler on PATH?");
    if (!runResults)
        log.error("Runner",
                  "Binary or Script won't be executed because its corresponding program or VM could not be loaded");
}

// -------------------- COMPILER SLOTS ---------------------------

void MainWindow::onCompilationStarted()
{
    log.info("Compiler", "Compilation has started");
}

void MainWindow::onCompilationFinished(const QString &warning)
{
    log.info("Compiler", "Compilation has finished");
    if (!warning.trimmed().isEmpty())
    {
        log.warn("Compile Warnings", warning.toStdString());
    }

    if (afterCompile == Run)
    {
        run();
    }
    else if (afterCompile == RunDetached)
    {
        killProcesses();

        QString command, args;
        if (language == "Cpp")
        {
            args = data.runtimeArgumentsCpp;
        }
        else if (language == "Java")
        {
            command = data.runCommandJava;
            args = data.runtimeArgumentsJava;
        }
        else if (language == "Python")
        {
            command = data.runCommandPython;
            args = data.runtimeArgumentsPython;
        }
        else
        {
            log.warn("Runner", "Wrong language, please set the language");
            return;
        }

        detachedRunner = new Core::Runner(-1);
        connect(detachedRunner, SIGNAL(runErrorOccured(int)), this, SLOT(onRunErrorOccured(int)));
        connect(detachedRunner, SIGNAL(runKilled(int)), this, SLOT(onRunKilled(int)));
        detachedRunner->runDetached(tmpPath(), language, command, args);
    }

    afterCompile = Nothing;
}

void MainWindow::onCompilationErrorOccured(const QString &error)
{
    log.error("Complier", "Error occured while compiling");
    if (!error.trimmed().isEmpty())
        log.error("Compile Errors", error.toStdString());
}

// --------------------- RUNNER SLOTS ----------------------------

std::string MainWindow::getRunnerHead(int index)
{
    if (index == -1)
        return "Detached Runner";
    return "Runner[" + std::to_string(index + 1) + "]";
}

void MainWindow::onRunStarted(int index)
{
    log.info(getRunnerHead(index), "Execution has started");
}

void MainWindow::onRunFinished(int index, const QString &out, const QString &err, int exitCode, int timeUsed)
{
    auto head = getRunnerHead(index);

    if (exitCode == 0)
    {
        log.info(head, "Execution for test case #" + std::to_string(index + 1) + " has finished in " +
                           std::to_string(timeUsed) + "ms");
        if (!err.trimmed().isEmpty())
            log.error(head + "/stderr", err.toStdString());
        output[index]->setPlainText(out);
        if (!expected[index]->isEmpty())
        {
            if (isVerdictPass(out, *expected[index]))
                updateVerdict(ACCEPTED, index);
            else
                updateVerdict(WRONG_ANSWER, index);
        }
    }

    else
    {
        log.error(head, "Execution for test case #" + std::to_string(index + 1) +
                            " has finished with non-zero exitcode " + std::to_string(exitCode) + " in " +
                            std::to_string(timeUsed) + "ms");
        if (!err.trimmed().isEmpty())
            log.error(head + "/stderr", err.toStdString());
    }
}

void MainWindow::onRunErrorOccured(int index, const QString &error)
{
    log.error(getRunnerHead(index), error.toStdString());
}

void MainWindow::onRunTimeout(int index)
{
    log.warn(getRunnerHead(index), "Time Limit Exceeded");
}

void MainWindow::onRunKilled(int index)
{
    log.info(getRunnerHead(index),
             (index == -1 ? "Detached runner" : "Runner for test case #" + std::to_string(index + 1)) +
                 " has been killed");
}
