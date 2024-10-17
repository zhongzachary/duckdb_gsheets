#include "gsheets_copy.hpp"
#include "gsheets_writer.hpp"
#include "duckdb/common/serializer/buffered_file_writer.hpp"
#include "duckdb/common/file_system.hpp"
#include <iostream>
namespace duckdb {

void GSheetCopyState::Initialize(ClientContext &context) {
    std::cout << "GSheetCopyState initialized successfully" << std::endl;
}

GSheetCopyFunction::GSheetCopyFunction() : CopyFunction("gsheet") {
	copy_to_bind = GSheetWriteBind;
	copy_to_initialize_global = GSheetWriteInitializeGlobal;
	copy_to_initialize_local = GSheetWriteInitializeLocal;
	copy_to_sink = GSheetWriteSink;
	copy_to_combine = GSheetWriteCombine;
	copy_to_finalize = GSheetWriteFinalize;
}

struct GSheetCopyGlobalState : public GlobalFunctionData {
	explicit GSheetCopyGlobalState(ClientContext &context) {
		copy_state.Initialize(context);
	}

	void Flush(GSheetWriter &writer) {
		file_writer->WriteData(writer.stream.GetData(), writer.stream.GetPosition());
	}

	void WriteChunk(DataChunk &chunk) {
		chunk.Flatten();
		GSheetWriter writer(copy_state);
		for (idx_t r = 0; r < chunk.size(); r++) {
			for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
				auto &col = chunk.data[c];
				//writer.WriteValue(col, r);
                std::cout << FlatVector::GetData<string_t>(col)[r].GetData() << ", ";
			}
            std::cout << std::endl;
		}
		Flush(writer);
	}

	void Flush() {
		// flush and close the file
		file_writer->Flush();
		file_writer.reset();
	}

public:
	unique_ptr<BufferedFileWriter> file_writer;
	GSheetCopyState copy_state;
};

struct GSheetWriteBindData : public TableFunctionData {};

unique_ptr<FunctionData> GSheetCopyFunction::GSheetWriteBind(ClientContext &context,
                                                                             CopyFunctionBindInput &input,
                                                                             const vector<string> &names,
                                                                             const vector<LogicalType> &sql_types) {
	return make_uniq<GSheetWriteBindData>();
}

unique_ptr<GlobalFunctionData>
GSheetCopyFunction::GSheetWriteInitializeGlobal(ClientContext &context, FunctionData &bind_data,
                                                                const string &file_path) {
	auto result = make_uniq<GSheetCopyGlobalState>(context);
	auto &fs = FileSystem::GetFileSystem(context);
	result->file_writer = make_uniq<BufferedFileWriter>(fs, file_path);
	return std::move(result);
}

unique_ptr<LocalFunctionData>
GSheetCopyFunction::GSheetWriteInitializeLocal(ExecutionContext &context, FunctionData &bind_data_p) {
	return make_uniq<LocalFunctionData>();
}

void GSheetCopyFunction::GSheetWriteSink(ExecutionContext &context, FunctionData &bind_data_p,
                                                         GlobalFunctionData &gstate_p, LocalFunctionData &lstate,
                                                         DataChunk &input) {
	auto &gstate = gstate_p.Cast<GSheetCopyGlobalState>();
	gstate.WriteChunk(input);
}

void GSheetCopyFunction::GSheetWriteCombine(ExecutionContext &context, FunctionData &bind_data,
                                                            GlobalFunctionData &gstate, LocalFunctionData &lstate) {
}

void GSheetCopyFunction::GSheetWriteFinalize(ClientContext &context, FunctionData &bind_data,
                                                             GlobalFunctionData &gstate_p) {
	auto &gstate = gstate_p.Cast<GSheetCopyGlobalState>();
	// write the footer and close the file
	gstate.Flush();
}

} // namespace duckdb